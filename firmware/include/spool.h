#pragma once
#include "capture.h"

// All SD and network operations run on the Arduino loop task.
enum class AppendResult {
    Saved,         // Durable on the card; remove the sample from the queue.
    Rejected,      // The sample cannot be encoded; drop it.
    StorageFault,  // Keep the sample queued, unmount and retry the card later.
};

// Mounts the card (dropping any stale mount first) and seals a leftover
// current.open. Safe to call repeatedly from a retry schedule.
bool mountSpool();
void unmountSpool();
AppendResult appendReading(const Reading& sample);
// False means the card state is unknown or the rename failed. Nothing is reset.
bool sealBatch();
bool uploadNext();
extern const char* spoolStatus;
