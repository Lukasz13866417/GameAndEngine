#include "scene_lists.hpp"

namespace editor_example {
SceneLists::SceneLists(SceneListHosts hosts)
    : instances_(hosts.instances,InstanceList::Filter::scene), regions_(hosts.regions,InstanceList::Filter::regions),
      blueprints_(hosts.blueprints), instance_flyout_(hosts.instance_flyout,"INSTANCE LIST","Close instance list"),
      blueprint_flyout_(hosts.blueprint_flyout,"BLUEPRINT LIST","Close blueprint list"),
      floating_instances_(instance_flyout_.body(),InstanceList::Filter::scene), floating_blueprints_(blueprint_flyout_.body()) {}
void SceneLists::close() { instance_flyout_.close(); blueprint_flyout_.close(); }
void SceneLists::catalog(const SceneListCatalog& view) {
    if(view.synchronize_document) {
        instances_.sync(view.instances,view.blueprints);
        regions_.sync(view.instances,view.blueprints);
        blueprints_.sync(view.blueprints);
        if(instance_flyout_.opened()) floating_instances_.sync(view.instances,view.blueprints);
        if(blueprint_flyout_.opened()) floating_blueprints_.sync(view.blueprints);
    }
    selection(view.selection,view.reveal);
}
void SceneLists::selection(const vng::editor::Selection<vng::u32>& selected,std::optional<vng::u32> reveal) {
    instances_.selection(selected); regions_.selection(selected); floating_instances_.selection(selected);
    if(reveal) {
        instances_.reveal(*reveal); regions_.reveal(*reveal);
        if(instance_flyout_.opened()) floating_instances_.reveal(*reveal);
    }
}
void SceneLists::layout(const SceneListPresentation& view) {
    instance_flyout_.layout(view.window,view.instances_anchor);
    blueprint_flyout_.layout(view.window,view.blueprints_anchor);
    blueprints_.layout();
    if(blueprint_flyout_.opened()) floating_blueprints_.layout();
}
SceneListsReply SceneLists::handle(const Browsing&,const SceneListsContext& context) {
    SceneListsReply reply;
    if(context.enabled) {
        enabled_=*context.enabled;
        instance_flyout_.enabled(enabled_); blueprint_flyout_.enabled(enabled_);
    }
    if(context.catalog) catalog(*context.catalog);
    if(context.presentation) { presentation_=context.presentation; layout(*presentation_); }
    if(context.input.close) close();
    if(context.input.reset_scroll) { instance_flyout_.body().scroll(0); blueprint_flyout_.body().scroll(0); }
    if(context.input.rename) {
        const auto& name=*context.input.rename;
        reply.renamed = instances_.rename(name.id,name.name);
        reply.renamed |= regions_.rename(name.id,name.name);
        floating_instances_.rename(name.id,name.name);
    }
    if(context.input.poll_flyouts && presentation_) {
        instance_flyout_.poll(context.input.events,presentation_->instances_button,enabled_);
        blueprint_flyout_.poll(context.input.events,presentation_->blueprints_button,enabled_);
    }
    if(!enabled_) return reply;
    if(context.input.toggle!=SceneListInput::Toggle::none && context.catalog && presentation_) {
        const bool instances=context.input.toggle==SceneListInput::Toggle::instances;
        const bool was_open=instances ? instance_flyout_.opened() : blueprint_flyout_.opened();
        close();
        if(!was_open) {
            if(instances) {
                floating_instances_.sync(context.catalog->instances,context.catalog->blueprints);
                floating_instances_.selection(context.catalog->selection);
                instance_flyout_.open();
            } else {
                floating_blueprints_.sync(context.catalog->blueprints);
                blueprint_flyout_.open();
            }
            layout(*presentation_);
        }
    }
    if(context.input.poll && !context.input.events.empty()) {
        reply.instance=instances_.poll(context.input.events);
        if(auto value=regions_.poll(context.input.events)) reply.instance=value;
        if(instance_flyout_.opened())
            if(auto value=floating_instances_.poll(context.input.events)) reply.instance=value;
        reply.blueprint=blueprints_.poll();
        if(blueprint_flyout_.opened())
            if(auto value=floating_blueprints_.poll()) reply.blueprint=value;
    }
    return reply;
}
DebugReport SceneLists::debug_report() const {
    const auto report=[](std::string name,const InstanceList& list) {
        const auto stats=list.stats();
        return DebugReport{.name=std::move(name),.role="retained instance rows",.situation="Browsing",
            .owned={{"rows",std::to_string(list.entries().size())},{"catalog synchronizations",std::to_string(stats.syncs)},
                {"rows created",std::to_string(stats.rows_created)},{"labels updated",std::to_string(stats.labels_updated)}}};
    };
    return {.name="lists",.role="instance, region and blueprint lists with shared flyouts",.situation="Browsing",
        .received={{"input allowed",debug_bool(enabled_)}},
        .owned={{"instance flyout open",debug_bool(instance_flyout_.opened())},{"blueprint flyout open",debug_bool(blueprint_flyout_.opened())}},
        .children={report("instances",instances_),report("regions",regions_),report("floating_instances",floating_instances_)}};
}
} // namespace editor_example
