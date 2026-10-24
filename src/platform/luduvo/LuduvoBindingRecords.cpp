#include "Platform/luduvo/LuduvoBindingRecords.hpp"

const std::vector<LuduvoBuiltinResourceBinding> luduvoBuiltinResourceBindings = {
    {"Instance.AddComponent", "name", "component", true, true},
    {"Instance.FindAncestorWithComponent", "name", "component"},
    {"Instance.HasComponent", "name", "component"},
    {"Instance.RemoveComponent", "name", "component", true, true},
    {"Query.With", "name", "component"},
    {"Query.Without", "name", "component"},
    {"game.Prefabs.Exists", "name", "prefab"},
    {"game.Prefabs.Id", "name", "prefab"},
    {"game.Prefabs.RevertAddedComponent", "name", "component"},
    {"game.Prefabs.Spawn", "name", "prefab"},
    {"game.World.Each", "component", "component"},
    {"game.World.HasComponent", "component", "component"},
    {"game.World.Query", "components", "component", true, true},
    {"game.World.Spawn", "components", "component", true, true},
    {"World.Each", "component", "component"},
    {"World.HasComponent", "component", "component"},
    {"World.Query", "components", "component", true, true},
    {"World.Spawn", "components", "component", true, true},
};

const std::vector<LuduvoColorBinding> luduvoBuiltinColorBindings = {
    {"Color3.new", {"r", "g", "b"}, ColorPresentationEncoding::Rgb},
    {"Color3.fromRGB", {"r", "g", "b"}, ColorPresentationEncoding::Rgb255},
    {"Color3.fromHSV", {"h", "s", "v"}, ColorPresentationEncoding::Hsv},
    {"Color3.fromHex", {"hex"}, ColorPresentationEncoding::Hex},
};
