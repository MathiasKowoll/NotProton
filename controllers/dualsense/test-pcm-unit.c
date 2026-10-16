#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "PCM.h"
int main(void) {
    for (int f = 0; f < 2; f++) {
        DSBResampler r;
        DSBResamplerInit(&r, 48000);
        double energy = 0;
        unsigned n = 0;
        for (int i = 0; i < 48000; i++) {
            float v = .5 * sin(2 * M_PI * (f ? 5000 : 50) * i / 48000.);
            int8_t out[2];
            if (DSBResample(&r, v, -v, out)) {
                assert(out[0] == -out[1]);
                if (n > 100) {
                    energy += out[0] * out[0];
                }
                n++;
            }
        }
        assert(n == 3000);
        double rms = sqrt(energy / 2899) / 127;
        if (f) {
            assert(rms < .01);
        } else {
            assert(rms > .34 && rms < .37);
        }
    }
    int8_t data[128];
    for (int i = 0; i < 128; i++) {
        data[i] = i - 64;
    }
    for (unsigned seq = 0; seq < 512; seq++) {
        for (int first = 0; first < 2; first++) {
            uint8_t b[206];
            unsigned n = DSBPCMReport(b, seq, seq, first, data);
            assert(n == (first ? 206 : 142));
            assert(b[1] == (seq % 16) * 16);
            assert(!memcmp(b + (first ? 13 : 9), data, 128));
            assert(b[4] == (first ? 0xfe : 0x62));
            assert(b[first ? 10 : 6] == (uint8_t)(seq * 2));
            unsigned crc = 0xffffffff;
            unsigned char seed = 0xa2;
            for (unsigned k = 0; k < n - 3; k++) {
                unsigned char v = k ? b[k - 1] : seed;
                crc ^= v;
                for (int j = 0; j < 8; j++) {
                    crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
                }
            }
            crc = ~crc;
            assert(b[n - 4] == (crc & 255) && b[n - 3] == ((crc >> 8) & 255) &&
                   b[n - 2] == ((crc >> 16) & 255) && b[n - 1] == (crc >> 24));
        }
    }
    // Rumble on the voice coils: silence stays silent, full motors reach full
    // scale, each side carries its own tone, and block edges do not click.
    DSBRumbleSynth synth = {{0, 0}, {0, 0}};
    int8_t block[128];
    assert(!DSBRumbleSynthBlock(&synth, 0, 0, block));
    for (int i = 0; i < 128; i++) {
        assert(block[i] == 0);
    }
    int peak[2] = {0, 0}, crossings[2] = {0, 0}, last[2] = {0, 0}, jump = 0;
    for (int b = 0; b < 141; b++) { // ~3 s at 3 kHz
        assert(DSBRumbleSynthBlock(&synth, b == 0 ? .5f : 1, 1, block));
        for (int i = 0; i < 64; i++) {
            for (int ch = 0; ch < 2; ch++) {
                int v = block[i * 2 + ch];
                peak[ch] = abs(v) > peak[ch] ? abs(v) : peak[ch];
                if ((v >= 0) != (last[ch] >= 0)) {
                    crossings[ch]++;
                }
                if (b > 0 && abs(v - last[ch]) > jump) {
                    jump = abs(v - last[ch]);
                }
                last[ch] = v;
            }
        }
    }
    assert(peak[0] >= 125 && peak[1] >= 125);
    double seconds = 141 * 64 / 3000.;
    assert(fabs(crossings[0] / (2 * seconds) - DSB_RUMBLE_LOW_HZ) < 3);
    assert(fabs(crossings[1] / (2 * seconds) - DSB_RUMBLE_HIGH_HZ) < 3);
    // One sample step of a 160 Hz full-scale sine at 3 kHz is at most ~43.
    assert(jump < 48);
    // A drop to zero ramps out within one block rather than cutting off.
    assert(DSBRumbleSynthBlock(&synth, 0, 0, block));
    assert(!DSBRumbleSynthBlock(&synth, 0, 0, block));
    assert(!DSBRumbleSynthBlock(&synth, NAN, -1, block));
    puts("==> dualsense PCM: waveform, antialias filter, sample rate, payload, "
         "independent CRC and sequence wrap; rumble tones, scale and ramps");
}
