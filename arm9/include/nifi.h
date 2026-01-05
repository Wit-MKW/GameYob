#pragma once

extern uint8_t linkReceivedData;

void saveNifi();
void enableNifi();
void disableNifi();
void updateNifi(int cycles);
void sendPacketByte(uint8_t byte);
