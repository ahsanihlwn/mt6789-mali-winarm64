/*
 * link_probe.cpp  -  the C++ half of the ARM64 link probe.
 *
 * This is plain C++: a class, a constructor, a template and a virtual method,
 * but no STL containers, no <mutex>, no exceptions and no RTTI-dependent
 * library calls. That is exactly the shape of most of Mesa's C++ (see
 * u_qsort.cpp, texcompress_astc.cpp): it needs a C++ compiler and nothing from
 * the C++ runtime library.
 *
 * Mesa's texcompress_astc_luts.cpp is the counter-example - it uses
 * std::mutex / std::unordered_map / std::vector and therefore *does* need
 * libcpmt.lib. tools/link-probe.ps1 documents both cases.
 *
 * SPDX-License-Identifier: MIT
 */

template <typename T>
static T triple(T v) { return v + v + v; }

class Counter {
public:
    Counter() : n_(0) {}
    virtual ~Counter() {}
    int add(int d) { return n_ += d; }
private:
    int n_;
};

class Doubler : public Counter {
public:
    int twice(int d) { return add(d) + add(d); }
};

extern "C" int cpp_probe_value(void)
{
    Doubler d;
    return triple(d.twice(5) - d.add(1));
}
