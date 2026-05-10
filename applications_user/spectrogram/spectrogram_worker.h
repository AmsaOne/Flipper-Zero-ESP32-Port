#pragma once

#include "spectrogram_app.h"

SpectrogramWorker* spectrogram_worker_alloc(SpectrogramApp* app);
void spectrogram_worker_free(SpectrogramWorker* worker);
void spectrogram_worker_start(SpectrogramWorker* worker);
void spectrogram_worker_stop(SpectrogramWorker* worker);
