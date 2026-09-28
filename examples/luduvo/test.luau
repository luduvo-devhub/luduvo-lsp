--!strict
-- Tested in Zed. Complete the unfinished game. on line 11 or try hovering any of the luduvo-speciifc data types.
local part = game.Prefabs.Spawn("Part")
if part then
    part.Position = Vector3.new(0, 10, 0)
    part.Anchored = true
    part.Color = Color3.new(0.2, 0.6, 1)
    game.Tags.Add(part, "Example")
end

local finishMe = game.

local query = world.Query("Position")
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
game.Tween
local ET = game.EventTable
local damage = ET("Damage", ET.ToServer, {{"target", ET.Entity}, {"amount", ET.F32}})
damage:Push(self, 10)

local camera = game.World.Camera -- Should be red if this is running on a server
if camera then
    camera.CameraLens.fovY = math.rad(90)
    camera.CameraOrbit.distance = 8
end

function Update(dt: number)
    print(dt)
end
