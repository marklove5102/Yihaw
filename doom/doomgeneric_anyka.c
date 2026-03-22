#include "doomgeneric.h"
#include "doomkeys.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <stdint.h>
#include <time.h>

#define YUV_BUFFER_FILENAME "/tmp/doom_yuv.bin"
#define YUV_BUFFER_SIZE 345600

// | right | left | forward | backward | fire | use | escape | enter |
#define KEY_STATUS_FILENAME "/tmp/keystatus.bin"
#define KEY_STATUS_SIZE 8

#define KEYQUEUE_SIZE 16

#define INPUT_WIDTH   320
#define INPUT_HEIGHT  200
#define OUTPUT_WIDTH  640
#define OUTPUT_HEIGHT 360
#define OUTPUT_TOTAL  (OUTPUT_WIDTH * OUTPUT_HEIGHT)

static unsigned short s_KeyQueue[KEYQUEUE_SIZE];
static unsigned int s_KeyQueueWriteIndex = 0;
static unsigned int s_KeyQueueReadIndex = 0;

typedef enum {
    ANYKA_RIGHT,
    ANYKA_LEFT,
    ANYKA_FORWARD,
    ANYKA_BACKWARD,
    ANYKA_FIRE,
    ANYKA_USE,
    ANYKA_ESCAPE,
    ANYKA_ENTER
} ANYKA_DOOM_KEY;

static char *shared_yuv_buffer;
static char *key_status;
static char *last_key_status;

// Frame-ready eventfd: consumer blocks on read(), producer write()s 1 after each frame
static int frame_ready_fd = -1;

// --- Lookup tables ---

// Y channel (positive contributions)
static uint8_t y_lookup_red[256];
static uint8_t y_lookup_green[256];
static uint8_t y_lookup_blue[256];

// U channel: new_u = 128 - u_neg_red[r] - u_neg_green[g] + u_pos_blue[b]
static uint8_t u_neg_lookup_red[256];
static uint8_t u_neg_lookup_green[256];
static uint8_t u_pos_lookup_blue[256];

// V channel: new_v = 128 + v_pos_red[r] - v_neg_green[g] - v_neg_blue[b]
static uint8_t v_pos_lookup_red[256];
static uint8_t v_neg_lookup_green[256];
static uint8_t v_neg_lookup_blue[256];

// Precomputed Y-axis input row mapping for the 200->360 scale (non-power-of-2)
static int y_row_map[OUTPUT_HEIGHT];

static void precomputeLookupTables(void)
{
    for (int i = 0; i < 256; ++i) {
        y_lookup_red[i]   = (uint8_t)(0.299 * i);
        y_lookup_green[i] = (uint8_t)(0.587 * i);
        y_lookup_blue[i]  = (uint8_t)(0.114 * i);

        // U: 128 - 0.147r - 0.289g + 0.436b
        u_neg_lookup_red[i]   = (uint8_t)(0.147 * i);
        u_neg_lookup_green[i] = (uint8_t)(0.289 * i);
        u_pos_lookup_blue[i]  = (uint8_t)(0.436 * i);

        // V: 128 + 0.615r - 0.515g - 0.100b
        v_pos_lookup_red[i]   = (uint8_t)(0.615 * i);
        v_neg_lookup_green[i] = (uint8_t)(0.515 * i);
        v_neg_lookup_blue[i]  = (uint8_t)(0.100 * i);
    }

    // Precompute the Y-axis row mapping (OUTPUT_HEIGHT -> INPUT_HEIGHT)
    // Avoids the multiply+divide per pixel for the non-power-of-2 Y scale
    for (int i = 0; i < OUTPUT_HEIGHT; ++i) {
        y_row_map[i] = (i * INPUT_HEIGHT / OUTPUT_HEIGHT) * INPUT_WIDTH;
    }
}

static unsigned char convertToDoomKey(unsigned int key)
{
    switch (key) {
    case ANYKA_ENTER:    return KEY_ENTER;
    case ANYKA_ESCAPE:   return KEY_ESCAPE;
    case ANYKA_LEFT:     return KEY_LEFTARROW;
    case ANYKA_RIGHT:    return KEY_RIGHTARROW;
    case ANYKA_FORWARD:  return KEY_UPARROW;
    case ANYKA_BACKWARD: return KEY_DOWNARROW;
    case ANYKA_FIRE:     return KEY_FIRE;
    case ANYKA_USE:      return KEY_USE;
    default:             return tolower(key);
    }
}

static void addKeyToQueue(int pressed, unsigned int keyCode)
{
    unsigned char key = convertToDoomKey(keyCode);
    unsigned short keyData = (pressed << 8) | key;
    s_KeyQueue[s_KeyQueueWriteIndex] = keyData;
    s_KeyQueueWriteIndex = (s_KeyQueueWriteIndex + 1) % KEYQUEUE_SIZE;
}

int DG_GetKey(int *pressed, unsigned char *doomKey)
{
    if (s_KeyQueueReadIndex == s_KeyQueueWriteIndex)
        return 0;

    unsigned short keyData = s_KeyQueue[s_KeyQueueReadIndex];
    s_KeyQueueReadIndex = (s_KeyQueueReadIndex + 1) % KEYQUEUE_SIZE;
    *pressed = keyData >> 8;
    *doomKey = keyData & 0xFF;
    return 1;
}

static int open_shared_file(const char *path, size_t size)
{
    int fd = open(path, O_CREAT | O_RDWR, 0666);
    if (fd == -1) { perror("open"); exit(1); }
    if (ftruncate(fd, size) == -1) { perror("ftruncate"); exit(1); }
    return fd;
}

static char *map_shared(int fd, size_t size)
{
    char *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) { perror("mmap"); exit(1); }
    return ptr;
}

void DG_Init(void)
{
    printf("Initing!\n");
    precomputeLookupTables();

    int fd;

    fd = open_shared_file(YUV_BUFFER_FILENAME, YUV_BUFFER_SIZE);
    shared_yuv_buffer = map_shared(fd, YUV_BUFFER_SIZE);
    close(fd);

    fd = open_shared_file(KEY_STATUS_FILENAME, KEY_STATUS_SIZE);
    key_status = map_shared(fd, KEY_STATUS_SIZE);
    close(fd);

    last_key_status = calloc(KEY_STATUS_SIZE, sizeof(uint8_t));
    if (!last_key_status) { perror("calloc"); exit(1); }

    // Create the frame-ready eventfd
    frame_ready_fd = eventfd(0, EFD_NONBLOCK);
    if (frame_ready_fd == -1) { perror("eventfd"); exit(1); }
}

void DG_DrawFrame(void)
{
    uint8_t *yuv = (uint8_t *)shared_yuv_buffer;
    uint8_t *U_plane = yuv + OUTPUT_TOTAL;
    uint8_t *V_plane = yuv + OUTPUT_TOTAL + OUTPUT_TOTAL / 4;

    // Process pairs of rows so each 2x2 output block maps to one input pixel.
    // Stride in output UV plane is OUTPUT_WIDTH/2.
    for (int i = 0; i < OUTPUT_HEIGHT; i += 2) {
        // Precomputed input row offset — both output rows sample from the same input row
        const int in_row0 = y_row_map[i];

        uint8_t *Y_row0 = yuv + i * OUTPUT_WIDTH;
        uint8_t *Y_row1 = yuv + (i + 1) * OUTPUT_WIDTH;
        uint8_t *U_row  = U_plane + (i / 2) * (OUTPUT_WIDTH / 2);
        uint8_t *V_row  = V_plane + (i / 2) * (OUTPUT_WIDTH / 2);

        for (int j = 0; j < OUTPUT_WIDTH; j += 2) {
            // X scale is exactly 2x, so right-shift suffices.
            // x_input is also used as the UV index (both are j/2).
            const int x_input = j >> 1;

            // Sample one input pixel for the 2x2 output block (nearest-neighbour)
            const uint32_t px = DG_ScreenBuffer[in_row0 + x_input];
            const uint8_t r = (uint8_t)(px >> 16);
            const uint8_t g = (uint8_t)(px >>  8);
            const uint8_t b = (uint8_t)(px);

            // Y (luma) — write 4 pixels across two adjacent rows
            const uint8_t luma = y_lookup_red[r] + y_lookup_green[g] + y_lookup_blue[b];
            Y_row0[j]     = luma;
            Y_row0[j + 1] = luma;
            Y_row1[j]     = luma;
            Y_row1[j + 1] = luma;

            // U & V — one value per 2x2 block
            U_row[x_input] = (uint8_t)(128 + u_pos_lookup_blue[b]
                                           - u_neg_lookup_red[r]
                                           - u_neg_lookup_green[g]);
            V_row[x_input] = (uint8_t)(128 + v_pos_lookup_red[r]
                                           - v_neg_lookup_green[g]
                                           - v_neg_lookup_blue[b]);
        }
    }

    // Signal consumer that a complete frame is ready (prevents torn reads)
    uint64_t one = 1;
    write(frame_ready_fd, &one, sizeof(one));

    // Check for key state changes — skip the per-byte loop entirely if nothing changed
    if (memcmp(key_status, last_key_status, KEY_STATUS_SIZE) != 0) {
        for (int i = 0; i < KEY_STATUS_SIZE; i++) {
            if (key_status[i] != last_key_status[i]) {
                addKeyToQueue(key_status[i] == 1 ? 1 : 0, i);
                last_key_status[i] = key_status[i];
            }
        }
    }
}

void DG_SleepMs(uint32_t ms)
{
    usleep(ms * 1000);
}

uint32_t DG_GetTicksMs(void)
{
    struct timeval tp;
    struct timezone tzp;
    gettimeofday(&tp, &tzp);
    return (tp.tv_sec * 1000) + (tp.tv_usec / 1000);
}

void DG_SetWindowTitle(const char *title)
{
    printf("Setting window title: %s\n", title);
}

int main(int argc, char **argv)
{
    printf("Running doom!\n");
    doomgeneric_Create(argc, argv);

    for (;;)
        doomgeneric_Tick();

    return 0;
}