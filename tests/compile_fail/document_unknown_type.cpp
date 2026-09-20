#include <vng/content/document.hpp>

struct Unknown {};

void force_instantiation(vng::content::NodeView node)
{
    (void)node.get<Unknown>("custom");
}
