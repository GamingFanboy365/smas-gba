// Placeholder sound: the ports read back what the game wrote.
#include "platform.h"
#include "audio.h"
static uint8_t s_ports[4];
void AudioInit(void) {}
void AudioStart(uint32_t spc_tag) {}
void AudioEndFrame(void) {}
void AudioVBlank(void) {}
void ApuWritePort(int port, uint8_t value) { s_ports[port] = value; }
uint8_t ApuReadPort(int port) { return 0; }
void RtlApuUpload(const uint8_t *p) {}
