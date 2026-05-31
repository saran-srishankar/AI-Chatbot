#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>

struct audio_t {
    volatile unsigned int control_reg;
    volatile unsigned char rarc;
    volatile unsigned char ralc;
    volatile unsigned char wsrc;
    volatile unsigned char wslc;
    volatile int left;
    volatile int right;
} __attribute__((packed));;

#endif