/* CPU reference for the GPU stress shader (STRESS_SRC in bedrock-gpu.c): the
 * same steps in the same order. Shared with the host tests. */
#include <stdint.h>

static inline uint32_t stress_ref(uint32_t i, uint32_t iters)
{
	uint32_t x = i * 2654435761u + 12345u;
	float f = 0;
	for (uint32_t k = 0; k < iters; k++) {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		x = x * 1664525u + 1013904223u;
		f += (float)(x & 1023u) * (float)((x >> 10) & 1023u);
		if (f >= 8388608.0f)
			f -= 8388608.0f;
	}
	return x ^ (uint32_t)f;
}
