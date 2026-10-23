/* Created by Rick KD0OSS */
#pragma once
extern "C" {
unsigned int golay24128_encode23127(unsigned int data);
unsigned int golay24128_encode24128(unsigned int data);
}
class CGolay24128 {
public:
    static unsigned int encode23127(unsigned int d) { return golay24128_encode23127(d); }
    static unsigned int encode24128(unsigned int d) { return golay24128_encode24128(d); }
};
