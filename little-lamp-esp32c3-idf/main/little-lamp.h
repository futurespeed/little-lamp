#include <stdint.h>

typedef enum {
    LAMP_MODE_NORMAL,
    LAMP_MODE_SLEEP,
    LAMP_MODE_COLOR
} lamp_mode_t;

typedef struct {
    lamp_mode_t mode;
    uint8_t brightnessA;
    uint8_t brightnessB;
    uint8_t brightness;
    uint8_t warm;
    uint8_t color;
} lamp_info_t;
