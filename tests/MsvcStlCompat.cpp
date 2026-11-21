#include <algorithm>

#if defined(_MSC_VER) && _MSC_VER < 1945
extern "C" void __cdecl __std_rotate(void* first, void* middle, void* last) noexcept
{
    auto* f = static_cast<unsigned char*>(first);
    auto* m = static_cast<unsigned char*>(middle);
    auto* l = static_cast<unsigned char*>(last);

    if (f == m || m == l)
    {
        return;
    }

    std::rotate(f, m, l);
}
#endif
