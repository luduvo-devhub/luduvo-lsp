--!strict
-- Tested in Zed. Complete game.Physics. or hover Vector3.new.
local part = game.Prefabs.Spawn("Part")
if part then
    part.Parent = game.World
    part.Position = Vector3.new(0, 10, 0)
    part.Anchored = true
    part.Color = Color3.new(0.2, 0.6, 1)
end

local query = game.World.Query("Position")
local count: number = query:Refresh()
for i = 1, count do
    local entity: Instance = query.Entity[i]
    print(entity.Name)
end

local hit: RaycastResult? = game.Physics.Raycast(Vector3.zero, Vector3.yAxis, 100)
if hit then
    print(hit.Position)
end

local damage = Event("Damage", ToServer, {{"target", Entity}, {"amount", F32}})
damage:Push(self, 10)
