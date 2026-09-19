--!strict
-- Tested in Zed. Complete game.Physics. or hover Vector3.new.
local part = game.Prefabs.Spawn("Part")
if part then
    part.Position = Vector3.new(0, 10, 0)
    part.Anchored = true
    part.Color = Color3.new(0.2, 0.6, 1)
    game.Tags.Add(part, "Example")
end

local query = game.World.Query("Position")
query:With("Health"):Without("Invulnerable")
local count: number = query:Refresh()
for i = 1, count do
    local entity: Instance = query.Entity[i]
    print(entity.Name)
end

local hit: RaycastResult? = game.Physics.Raycast(Vector3.zero, Vector3.yAxis, 100)
if hit then
    print(hit.Position)
end

local ET = game.EventTable
local damage = ET("Damage", ET.ToServer, {{"target", ET.Entity}, {"amount", ET.F32}})
damage:Push(self, 10)

local camera = game.World.Camera
if camera then
    camera.CameraLens.fovY = math.rad(90)
    camera.CameraOrbit.distance = 8
end

function Update(dt: number)
    print(dt)
end
