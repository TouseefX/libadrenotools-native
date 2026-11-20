#include "../src/turnip_compat.h"
#include <cassert>

int main() {
    assert(parse_adreno_generation("Adreno (TM) 650") == 6);
    assert(parse_adreno_generation("adreno619") == 6);
    assert(parse_adreno_generation("SM8250 Adreno 650") == 6);
    assert(parse_adreno_generation("650") == 6);
    assert(parse_adreno_generation("Adreno 6") == 0);
    assert(parse_adreno_generation("Adreno 6500") == 0);
    assert(parse_adreno_generation("Adreno 6650") == 0);
    assert(parse_adreno_generation("Adreno 530") == 5);
    assert(parse_adreno_generation("Adreno 740") == 7);
    assert(parse_adreno_generation("Adreno 830") == 8);
    assert(parse_adreno_generation(nullptr) == 0);
    assert(parse_adreno_generation("") == 0);
    assert(parse_adreno_generation("a") == 0);
    assert(parse_adreno_generation("adren") == 0);
    return 0;
}
