#!/usr/bin/env python3
"""把 leejet 的 Metal FP8 cast 支持搬进上游 ggml 的 kernels/ 布局。

上游把单体 ggml-metal.metal 拆成了 kernels/*.metal，leejet 的 fork 还是老布局，
所以这一段不能机械 patch。cpy/cast kernel 在上游住在 kernels/quantize.metal。
"""
import pathlib, sys
import sys

# **Windows 上标准输出默认不是 UTF-8。**
#
# GitHub 的 windows runner 跑 Python 时控制台编码是 cp1252，而下面那些进度
# 是中文——print 到一半直接 UnicodeEncodeError，脚本非零退出。CMake 那边
# 报的是"patch step 失败"、MSBuild 报 MSB8066，**和真正的原因（编码）
# 差着十万八千里**，日志里要翻到最底下才看得见那行 UnicodeEncodeError。
# 2026-09-11 六平台流水线头一次跑，windows-x64 和 windows-arm64 两格就
# 挂在这儿，另外四个平台全过。
#
# errors="replace"：宁可某个字打成问号，也不能因为一个字让整个构建挂掉。
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # 老 Python 或者被重定向成不支持的对象
        pass


f = pathlib.Path("src/ggml-metal/kernels/quantize.metal")
s = f.read_text(encoding="utf-8")

HELPERS = '''template<typename T1, typename T0>
static inline T1 cpy_cast(T0 value) {
    return (T1) value;
}

struct ggml_fp8_e4m3_metal {
    uint8_t value;
};

struct ggml_fp8_e5m2_metal {
    uint8_t value;
};

static inline float fp8_e4m3_to_fp32(uint8_t x) {
    const uint32_t sign     = x >> 7;
    const uint32_t exponent = (x >> 3) & 0x0f;
    const uint32_t mantissa = x & 0x07;

    if (exponent == 0x0f && mantissa == 0x07) {
        return as_type<float>((sign << 31) | 0x7fc00000);
    }

    float value;
    if (exponent == 0) {
        value = float(mantissa) * (1.0f / 512.0f);
    } else {
        value = (1.0f + float(mantissa) / 8.0f) * exp2(float(int(exponent) - 7));
    }
    return sign ? -value : value;
}

static inline float fp8_e5m2_to_fp32(uint8_t x) {
    const uint32_t sign     = x >> 7;
    const uint32_t exponent = (x >> 2) & 0x1f;
    const uint32_t mantissa = x & 0x03;

    if (exponent == 0x1f) {
        const uint32_t fp32_bits = mantissa == 0 ? 0x7f800000 : 0x7fc00000;
        return as_type<float>((sign << 31) | fp32_bits);
    }

    float value;
    if (exponent == 0) {
        value = float(mantissa) * (1.0f / 65536.0f);
    } else {
        value = (1.0f + float(mantissa) / 4.0f) * exp2(float(int(exponent) - 15));
    }
    return sign ? -value : value;
}

template<typename T1>
static inline T1 cpy_cast(ggml_fp8_e4m3_metal value) {
    return (T1) fp8_e4m3_to_fp32(value.value);
}

template<typename T1>
static inline T1 cpy_cast(ggml_fp8_e5m2_metal value) {
    return (T1) fp8_e5m2_to_fp32(value.value);
}

'''

INSTANTIATIONS = '''
template [[host_name("kernel_cpy_f8_e4m3_f16")]]  kernel kernel_cpy_t kernel_cpy_t_t<ggml_fp8_e4m3_metal, half>;
template [[host_name("kernel_cpy_f8_e5m2_f16")]]  kernel kernel_cpy_t kernel_cpy_t_t<ggml_fp8_e5m2_metal, half>;
template [[host_name("kernel_cpy_f8_e4m3_bf16")]] kernel kernel_cpy_t kernel_cpy_t_t<ggml_fp8_e4m3_metal, bfloat>;
template [[host_name("kernel_cpy_f8_e5m2_bf16")]] kernel kernel_cpy_t kernel_cpy_t_t<ggml_fp8_e5m2_metal, bfloat>;
'''

if "ggml_fp8_e4m3_metal" in s:
    print("已经移植过，跳过"); sys.exit(0)

# 1) 辅助块插到 kernel_cpy_t_t 定义之前
anchor = "kernel void kernel_cpy_t_t("
i = s.index(anchor)
# 回退到该行所属的 template<> 声明行首
j = s.rfind("template<", 0, i)
assert j != -1, "找不到 kernel_cpy_t_t 的 template 声明"
s = s[:j] + HELPERS + s[j:]

# 2) 赋值改成走 cpy_cast
old_assign = "dst_data[i00] = (T1) src[0];"
assert s.count(old_assign) == 1, f"赋值行出现 {s.count(old_assign)} 次，预期 1 次"
s = s.replace(old_assign, "dst_data[i00] = cpy_cast<T1>(src[0]);")

# 3) 四个实例化追加到最后一个 kernel_cpy_ 实例化之后
last = s.rindex('template [[host_name("kernel_cpy_')
end = s.index("\n", last) + 1
s = s[:end] + INSTANTIATIONS + s[end:]

f.write_text(s, encoding="utf-8")
print("移植完成：辅助块 + 赋值改写 + 4 个实例化")
