#include "../src/turnip_compat.h"
#include <cassert>

int main() {
    assert(is_adreno_650_model("Adreno (TM) 650"));
    assert(is_adreno_650_model("adreno650"));
    assert(!is_adreno_650_model(nullptr));
    assert(!is_adreno_650_model(""));
    assert(!is_adreno_650_model("Snapdragon 8650"));
    assert(!is_adreno_650_model("Adreno 6500"));
    assert(!is_adreno_650_model("Adreno 6650"));
    assert(!is_adreno_650_model("Adreno 740"));
    unsetenv("TU_DEBUG");
    assert(!set_adreno_650_turnip_default("Adreno 740"));
    assert(!std::getenv("TU_DEBUG"));
    assert(set_adreno_650_turnip_default("Adreno (TM) 650"));
    assert(std::strcmp(std::getenv("TU_DEBUG"), "noubwc") == 0);
    setenv("TU_DEBUG", "sysmem", 1);
    assert(!set_adreno_650_turnip_default("Adreno (TM) 650"));
    assert(std::strcmp(std::getenv("TU_DEBUG"), "sysmem") == 0);
    setenv("TU_DEBUG", "", 1);
    assert(!set_adreno_650_turnip_default("Adreno (TM) 650"));
    assert(std::strcmp(std::getenv("TU_DEBUG"), "") == 0);
    return 0;
}
