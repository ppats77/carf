#pragma once
#include <Arduino.h>
#include <atomic>
#include "fleet.h"

struct Reading {
    char trip[37];
    Vehicle vehicle;
    char request[7];      // Optional supplemental diagnostic observation.
    char response[448];   // Adapter ASCII, including reply headers on 11-bit CAN.
    bool canHeaders;
    uint64_t observationMs;
    uint64_t elapsed;
    int64_t utc;
    int rpmQuarter;  // Raw two-byte SAE value, -1 means unavailable.
    int speed;
    int coolant;     // -1000 means unavailable.
};

extern std::atomic<Vehicle> selectedVehicle;
extern QueueHandle_t readings;
extern std::atomic<bool> captureEnabled;
extern std::atomic<bool> forgetPeer;
extern std::atomic<bool> utcKnown;
extern std::atomic<const char*> captureStatus;

void makeUuid(char (&uuid)[37]);
void captureTask(void*);
