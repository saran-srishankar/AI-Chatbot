#include <string.h>
#include "address_map.h"
#include "audio.h"

// JTAG UART
#define JTAG_DATA  (*(volatile int *)(JTAG_UART_BASE))
#define JTAG_CTRL  (*(volatile int *)(JTAG_UART_BASE + 4))

// PS/2
#define PS2_DATA_REG (*(volatile int *)(PS2_BASE))

// VGA character buffer
#define VGA_CHAR(row, col) (*(volatile char *)(FPGA_CHAR_BASE + (row)*128 + (col)))
#define VGA_COLS 80
#define VGA_ROWS 60

// Protocol headers
#define MSG_TEXT  0x01
#define MSG_AUDIO 0x02

// Audio
#define AUDIO_BASE 0xff203040

// Buffers
char input_buf[300];
int  input_len = 0;
char result_buf[300];

// ========================= 
// VGA
// =========================
void vga_clear() {
    for (int r = 0; r < VGA_ROWS; r++)
        for (int c = 0; c < VGA_COLS; c++)
            VGA_CHAR(r, c) = ' ';
}

void vga_write_str(int row, int col, const char *s) {
    while (*s) {
        if (col >= VGA_COLS) {
            col = 0;
            row++;  // Move to the next line when we reach the end of the current line
        }
        if (row >= VGA_ROWS) {
            // If the row exceeds the screen, shift all content up by one row
            for (int r = 1; r < VGA_ROWS; r++) {
                for (int c = 0; c < VGA_COLS; c++) {
                    VGA_CHAR(r - 1, c) = VGA_CHAR(r, c);
                }
            }
            // Clear the last row
            for (int c = 0; c < VGA_COLS; c++) {
                VGA_CHAR(VGA_ROWS - 1, c) = ' ';
            }
            row = VGA_ROWS - 1;  // Keep it at the last row
        }
        VGA_CHAR(row, col++) = *s++;
    }
}

void vga_render() {
    vga_clear();
    vga_write_str(0,  0, "Back to The Future - LLM");
    vga_write_str(2,  0, "Final Presentation - Punya Syon Pandey and Saran Srishankar");
    vga_write_str(4,  0, "----------------------------------------");
    vga_write_str(6,  0, "INPUT:");
    vga_write_str(6,  7, input_buf);
    vga_write_str(10, 0, "----------------------------------------");
    vga_write_str(11,  0, "REPLY:");
    vga_write_str(11,  8, result_buf);
    vga_write_str(15,  0, "----------------------------------------");
    char prompt[83] = "> ";
    strncat(prompt, input_buf, 78);
    vga_write_str(17, 0, prompt);
    vga_write_str(19, 0, "Type a message and press Enter to send ");
}

// =========================
// JTAG UART
// =========================
void jtag_send_char(char c) {
    while (!((JTAG_CTRL >> 16) & 0xFFFF));
    JTAG_DATA = c;
}

void jtag_send_str(const char *s) {
    while (*s) jtag_send_char(*s++);
    jtag_send_char('\n');
}

// Read one byte from JTAG, blocking
unsigned char jtag_recv_blocking() {
    int val;
    do { val = JTAG_DATA; } while (!(val & 0x8000));
    return (unsigned char)(val & 0xFF);
}

// Non-blocking JTAG read, returns -1 if nothing
int jtag_recv_byte() {
    int val = JTAG_DATA;
    if (val & 0x8000) return val & 0xFF;
    return -1;
}

int result_cursor = 0;
int waiting_for_response = 0;

// =========================
// PS/2
// =========================
static int ps2_shift = 0;

static const unsigned char sc_to_ascii[256] = {
    [0x1C]='a',[0x32]='b',[0x21]='c',[0x23]='d',[0x24]='e',[0x2B]='f',
    [0x34]='g',[0x33]='h',[0x43]='i',[0x3B]='j',[0x42]='k',[0x4B]='l',
    [0x3A]='m',[0x31]='n',[0x44]='o',[0x4D]='p',[0x15]='q',[0x2D]='r',
    [0x1B]='s',[0x2C]='t',[0x3C]='u',[0x2A]='v',[0x1D]='w',[0x22]='x',
    [0x35]='y',[0x1A]='z',
    [0x45]='0',[0x16]='1',[0x1E]='2',[0x26]='3',[0x25]='4',[0x2E]='5',
    [0x36]='6',[0x3D]='7',[0x3E]='8',[0x46]='9',
    [0x4E]='-',[0x55]='=',[0x4C]=';',[0x52]='\'',[0x41]=',',[0x49]='.',
    [0x4A]='/',[0x29]=' ',[0x5A]='\n',[0x66]='\b',
};

static const unsigned char sc_to_ascii_shift[256] = {
    [0x1C]='A',[0x32]='B',[0x21]='C',[0x23]='D',[0x24]='E',[0x2B]='F',
    [0x34]='G',[0x33]='H',[0x43]='I',[0x3B]='J',[0x42]='K',[0x4B]='L',
    [0x3A]='M',[0x31]='N',[0x44]='O',[0x4D]='P',[0x15]='Q',[0x2D]='R',
    [0x1B]='S',[0x2C]='T',[0x3C]='U',[0x2A]='V',[0x1D]='W',[0x22]='X',
    [0x35]='Y',[0x1A]='Z',
    [0x45]=')',[0x16]='!',[0x1E]='@',[0x26]='#',[0x25]='$',[0x2E]='%',
    [0x36]='^',[0x3D]='&',[0x3E]='*',[0x46]='(',
    [0x4E]='_',[0x55]='+',[0x4C]=':',[0x52]='"',[0x41]='<',[0x49]='>',
    [0x4A]='?',[0x29]=' ',[0x5A]='\n',[0x66]='\b',
};

char ps2_get_char() {
    int reg = PS2_DATA_REG;
    if (!(reg & 0x8000)) return 0;
    unsigned char byte = reg & 0xFF;
    if (byte == 0xF0) {
        while (!(PS2_DATA_REG & 0x8000));
        unsigned char released = PS2_DATA_REG & 0xFF;
        if (released == 0x12 || released == 0x59) ps2_shift = 0;
        return 0;
    }
    if (byte == 0x12 || byte == 0x59) { ps2_shift = 1; return 0; }
    if (byte == 0xE0) { while (!(PS2_DATA_REG & 0x8000)); return 0; }
    return ps2_shift ? sc_to_ascii_shift[byte] : sc_to_ascii[byte];
}

// =========================
// AUDIO
// =========================
void play_audio_block(unsigned int num_samples) {
    struct audio_t *audio_ptr = (struct audio_t *)(AUDIO_BASE);
    for (unsigned int i = 0; i < num_samples; i++) {
        unsigned char sample_u8 = jtag_recv_blocking();

        // Convert 8-bit unsigned to 32-bit signed for codec
        int32_t sample_32 = ((int32_t)(sample_u8 - 128)) << 24;
        while (audio_ptr->wsrc == 0 || audio_ptr->wslc == 0);
        audio_ptr->left  = sample_32;
        audio_ptr->right = sample_32;
    }
}
// =========================
// MAIN
// =========================
int main(void) {
    memset(input_buf,  0, sizeof(input_buf));
    memset(result_buf, 0, sizeof(result_buf));
    strncpy(result_buf, "---", 3);

    int  recv_len = 0;
    char recv_buf[300];
    memset(recv_buf, 0, sizeof(recv_buf));

    vga_render();

    while (1) {

        // --- PS/2 keyboard input ---
        char c = ps2_get_char();
        if (c) {
            if (c == '\n' || c == '\r') {
                if (input_len > 0) {
                    input_buf[input_len] = '\0';
                    strncpy(result_buf, "Waiting...", 79);
                    waiting_for_response = 1;
                    jtag_send_str(input_buf);
                    recv_len = 0;
                    vga_render();
                }
            } else if (c == '\b') {
                if (input_len > 0) input_buf[--input_len] = '\0';
                vga_render();
            } else if (input_len < 297) {
                input_buf[input_len++] = c;
                input_buf[input_len]   = '\0';
                vga_render();
            }
        }

        // --- JTAG receive (framed protocol) ---
        int b = jtag_recv_byte();
        if (b != -1) {
            unsigned char header = (unsigned char)b;

            if (header == MSG_TEXT) {
                if (waiting_for_response) {
                    memset(result_buf, 0, sizeof(result_buf));
                    result_cursor = 0;
                    waiting_for_response = 0;  // only reset once per response
                }
                unsigned char rc = jtag_recv_blocking();
                while (rc != '\n') {
                    if (rc != '\0' && result_cursor < 299) {
                        result_buf[result_cursor++] = (char)rc;
                        result_buf[result_cursor] = '\0';
                    }
                    rc = jtag_recv_blocking();
                }
                vga_render();
            } else if (header == MSG_AUDIO) {
                    // Read 4-byte sample count (little-endian)
                    unsigned int num_samples = 0;
                    num_samples |= (unsigned int)jtag_recv_blocking();
                    num_samples |= (unsigned int)jtag_recv_blocking() << 8;
                    num_samples |= (unsigned int)jtag_recv_blocking() << 16;
                    num_samples |= (unsigned int)jtag_recv_blocking() << 24;

                    play_audio_block(num_samples);
                }
        }
    }

    return 0;
}