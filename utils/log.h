#pragma once
#include <stdio.h>
#define LOG_ERROR(fmt, ...) fprintf(stderr, "CBOR: " fmt "\n", ##__VA_ARGS__)
