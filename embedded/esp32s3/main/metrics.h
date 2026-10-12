// Metrics to the house VictoriaMetrics (CRYPT-28): the key's health every minute, for spotting
// what the serial log can't show once it's on the native port (above all RAM, its real limit).
#pragma once

#include "prov.h"

// Starts the metrics task if the key is provisioned with a metrics URL (prov's metrics_*); does
// nothing otherwise. Best effort throughout: never blocks CTAP, presence or updates.
void metrics_start(const prov_t *prov);
