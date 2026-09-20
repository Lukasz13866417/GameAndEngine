#include <vng/content/document.hpp>

void force_instantiation(vng::content::NodeView node)
{
    (void)node.read([](vng::content::Reader& reader) -> vng::content::Reader& {
        return reader;
    });
}
