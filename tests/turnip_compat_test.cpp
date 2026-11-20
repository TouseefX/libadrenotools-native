#include "../src/turnip_compat.h"
#include <cassert>
#include <cstring>

int main() {
    assert(is_adreno_6xx_model("Adreno (TM) 650"));
    assert(is_adreno_6xx_model("adreno650"));
    assert(is_adreno_6xx_model("Adreno (TM) 619"));
    assert(is_adreno_6xx_model("Adreno 630"));
    assert(is_adreno_6xx_model("Adreno 600"));
    assert(is_adreno_6xx_model("Adreno 699"));
    assert(!is_adreno_6xx_model(nullptr));
    assert(!is_adreno_6xx_model(""));
    assert(!is_adreno_6xx_model("Snapdragon 8650"));
    assert(!is_adreno_6xx_model("Adreno 6500"));
    assert(!is_adreno_6xx_model("Adreno 6650"));
    assert(!is_adreno_6xx_model("Adreno 60"));
    assert(!is_adreno_6xx_model("Adreno 530"));
    assert(!is_adreno_6xx_model("Adreno 740"));
    assert(!is_adreno_6xx_model("Adreno 740 driver 650"));
    unsetenv("TU_DEBUG");
    assert(!set_adreno_6xx_turnip_default("Adreno 740"));
    assert(!std::getenv("TU_DEBUG"));
    assert(set_adreno_6xx_turnip_default("Adreno (TM) 619"));
    assert(std::strcmp(std::getenv("TU_DEBUG"), "noubwc") == 0);
    // The restored legacy default must not overwrite the workaround.
    setenv("TU_DEBUG", "noconform", 0);
    assert(std::strcmp(std::getenv("TU_DEBUG"), "noubwc") == 0);
    setenv("TU_DEBUG", "sysmem", 1);
    assert(!set_adreno_6xx_turnip_default("Adreno (TM) 650"));
    assert(std::strcmp(std::getenv("TU_DEBUG"), "sysmem") == 0);
    setenv("TU_DEBUG", "", 1);
    assert(!set_adreno_6xx_turnip_default("Adreno (TM) 650"));
    assert(std::strcmp(std::getenv("TU_DEBUG"), "") == 0);
    return 0;
}
