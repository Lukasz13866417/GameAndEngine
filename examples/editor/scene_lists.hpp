#pragma once
#include "component_debug.hpp"
#include "instance_list.hpp"
#include "blueprint_list.hpp"
#include "panel_flyout.hpp"

namespace editor_example {
struct SceneListHosts {
    vng::ui::Container instances, regions, blueprints, instance_flyout, blueprint_flyout;
};
struct SceneListCatalog {
    std::span<const SceneInstance> instances;
    std::span<const Blueprint> blueprints;
    const vng::editor::Selection<vng::u32>& selection;
    bool synchronize_document{true};
    std::optional<vng::u32> reveal{};
};
struct SceneListPresentation {
    vng::Vec2 window{};
    vng::ui::Rect instances_anchor{}, blueprints_anchor{};
    vng::ui::Rect instances_button{}, blueprints_button{};
};
struct SceneListInput {
    enum class Toggle { none, instances, blueprints };
    std::span<const vng::input::Event> events{};
    Toggle toggle{Toggle::none};
    bool poll{}, close{}, reset_scroll{}, poll_flyouts{};
    struct Rename { vng::u32 id; std::string_view name; };
    std::optional<Rename> rename{};
};
struct SceneListsContext {
    std::optional<SceneListCatalog> catalog{};
    std::optional<SceneListPresentation> presentation{};
    SceneListInput input{};
    std::optional<bool> enabled{};
};
struct SceneListsReply {
    std::optional<InstanceList::Pick> instance{};
    std::optional<BlueprintList::Action> blueprint{};
    bool renamed{};
};
// Both visual hosts are children of one logical component. Moving a list into
// a flyout never gives that flyout its own model or selection authority.
class SceneLists final {
public:
    struct Browsing {};
    explicit SceneLists(SceneListHosts);
    SceneLists(const SceneLists&) = delete;
    SceneLists& operator=(const SceneLists&) = delete;
    [[nodiscard]] bool contains(vng::u32 id) const { return instances_.contains(id)||regions_.contains(id); }
    [[nodiscard]] bool flyout_contains(vng::Vec2 point) const { return instance_flyout_.contains(point)||blueprint_flyout_.contains(point); }
    [[nodiscard]] bool flyout_open() const { return instance_flyout_.opened()||blueprint_flyout_.opened(); }
    [[nodiscard]] DebugReport debug_report() const;
    [[nodiscard]] std::string debug_string() const { return debug_report().string(); }
private:
    friend class EditingWorkspace;
    SceneListsReply handle(const Browsing&,const SceneListsContext&);
    void close();
    void catalog(const SceneListCatalog&);
    void selection(const vng::editor::Selection<vng::u32>&,std::optional<vng::u32> reveal={});
    void layout(const SceneListPresentation&);
    InstanceList instances_, regions_;
    BlueprintList blueprints_;
    PanelFlyout instance_flyout_, blueprint_flyout_;
    InstanceList floating_instances_;
    BlueprintList floating_blueprints_;
    bool enabled_{true};
    std::optional<SceneListPresentation> presentation_;
};
} // namespace editor_example
