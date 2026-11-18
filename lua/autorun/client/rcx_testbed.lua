--[[
	The Client Expansion's VR testbed: the frame loop of the VR design (wait, poses, two
	render.RenderView calls into the eye texture, submit, return true from RenderScene) on its own,
	in sandbox on a stock map, with no gamemode code. Nothing loads or hooks until a command runs.

	rcx_status                         the module's version, features, disabled features and VR stats
	rcx_vr_test_start [scene|pattern]  start VR and the frame loop; pattern draws a fixed test card
	rcx_vr_test_mode scene|pattern     switch while running
	rcx_vr_test_stop                   stop the frame loop and VR
]]

local unitsPerMeter = CreateClientConVar("rcx_vr_test_scale", "52.49", true, false, "Testbed world units per real meter")
local verbose = CreateClientConVar("rcx_vr_test_verbose", "1", true, false, "Testbed prints frame timings every 5 seconds")
CreateClientConVar("rcx_vr", "1", true, false, "Allow the Client Expansion's VR component")

local Testbed = RCXTestbed or {}
RCXTestbed = Testbed

Testbed.running = false
Testbed.mode = Testbed.mode or "scene"
Testbed.materials = Testbed.materials or {}

local function Message(text)
	MsgC(Color(120, 200, 255), "[rcx testbed] ", color_white, text, "\n")
end

---Loads the module once. Returns false and a reason when it isn't installed or fails to load.
---@return boolean loaded
---@return string? reason
local function LoadModule()
	if (rcx) then return true end

	if (not util.IsBinaryModuleInstalled("rcx")) then
		return false, "gmcl_rcx_win64.dll isn't in garrysmod/lua/bin"
	end

	local loaded, failure = pcall(require, "rcx")

	if (not loaded) then
		return false, tostring(failure)
	end

	if (not rcx) then
		return false, "require succeeded but the rcx table is missing"
	end

	return true
end

local function JoinKeys(map)
	local keys = {}

	for key, value in SortedPairs(map) do
		keys[#keys + 1] = isstring(value) and (key .. " (" .. value .. ")") or key
	end

	return #keys > 0 and table.concat(keys, ", ") or "none"
end

local function PrintStats()
	local stats = rcx.vr.GetStats()

	for key, value in SortedPairs(stats) do
		Message(string.format("  %s = %s", key, tostring(value)))
	end
end

--[[
	The eye frustums are asymmetric. Each eye renders one symmetric frustum wide enough to hold its
	real one, and the submitted bounds crop that back to the real frustum. Both eyes share one size.
]]
local function BuildLayout(display)
	local tanX, tanY = 0, 0

	for _, eye in ipairs({display.left, display.right}) do
		tanX = math.max(tanX, eye.tanLeft, eye.tanRight)
		tanY = math.max(tanY, eye.tanUp, eye.tanDown)
	end

	local widestEye = math.max(display.left.tanLeft + display.left.tanRight, display.right.tanLeft + display.right.tanRight)
	local tallestEye = math.max(display.left.tanUp + display.left.tanDown, display.right.tanUp + display.right.tanDown)
	local eyeWidth = math.ceil(display.width * (2 * tanX) / widestEye / 2) * 2
	local eyeHeight = math.ceil(display.height * (2 * tanY) / tallestEye / 2) * 2

	local layout = {
		eyeWidth = eyeWidth,
		width = eyeWidth * 2,
		height = eyeHeight,
		fov = math.deg(2 * math.atan(tanX)),
		aspect = tanX / tanY,
		bounds = {}
	}

	for index, name in ipairs({"left", "right"}) do
		local eye = display[name]
		local offset = (index - 1) * 0.5

		layout.bounds[name] = {
			uMin = offset + (tanX - eye.tanLeft) / (2 * tanX) * 0.5,
			uMax = offset + (tanX + eye.tanRight) / (2 * tanX) * 0.5,
			vMin = (tanY - eye.tanUp) / (2 * tanY),
			vMax = (tanY + eye.tanDown) / (2 * tanY)
		}
	end

	return layout
end

local function DrawPattern(layout, frameId)
	render.Clear(0, 0, 0, 255, true, true)
	cam.Start2D()

	for index, name in ipairs({"left", "right"}) do
		local left = (index - 1) * layout.eyeWidth
		local bounds = layout.bounds[name]
		local centerX = math.floor((bounds.uMin + bounds.uMax) * 0.5 * layout.width)
		local centerY = math.floor((bounds.vMin + bounds.vMax) * 0.5 * layout.height)

		surface.SetDrawColor(index == 1 and Color(90, 20, 20) or Color(20, 20, 90))
		surface.DrawRect(left, 0, layout.eyeWidth, layout.height)

		surface.SetDrawColor(80, 80, 80)

		for gridX = left, left + layout.eyeWidth, 100 do
			surface.DrawRect(gridX, 0, 2, layout.height)
		end

		for gridY = 0, layout.height, 100 do
			surface.DrawRect(left, gridY, layout.eyeWidth, 2)
		end

		surface.SetDrawColor(255, 255, 0)
		surface.DrawRect(centerX - 60, centerY - 2, 120, 4)
		surface.DrawRect(centerX - 2, centerY - 60, 4, 120)

		surface.SetDrawColor(0, 255, 0)
		surface.DrawRect(left + (frameId % 90) / 90 * layout.eyeWidth, centerY + 200, 40, 40)

		draw.SimpleText(name == "left" and "LEFT" or "RIGHT", "DermaLarge", centerX, centerY - 120, color_white, TEXT_ALIGN_CENTER, TEXT_ALIGN_CENTER)
	end

	cam.End2D()
end

local function RenderEyes(layout, poses, display)
	local scale = unitsPerMeter:GetFloat()
	local player = LocalPlayer()
	local head = poses.head

	if (not Testbed.originYaw) then
		Testbed.originYaw = (IsValid(player) and player:EyeAngles().y or 0) - head.ang.y
	end

	local trackingOrigin = IsValid(player) and player:GetPos() or vector_origin
	local headPos, headAng = LocalToWorld(head.pos * scale, head.ang, trackingOrigin, Angle(0, Testbed.originYaw, 0))

	render.Clear(0, 0, 0, 255, true, true)

	for index, name in ipairs({"left", "right"}) do
		local eye = display[name]
		local eyePos, eyeAng = LocalToWorld(eye.pos * scale, eye.ang, headPos, headAng)

		render.RenderView({
			origin = eyePos,
			angles = eyeAng,
			fov = layout.fov,
			aspectratio = layout.aspect,
			x = (index - 1) * layout.eyeWidth,
			y = 0,
			w = layout.eyeWidth,
			h = layout.height,
			znear = 1,
			drawviewmodel = false,
			drawhud = false
		})
	end
end

local function DrawDesktop(layout)
	local bounds = layout.bounds.left

	render.Clear(0, 0, 0, 255, true, true)
	cam.Start2D()
	surface.SetDrawColor(255, 255, 255)
	surface.SetMaterial(Testbed.material)

	local screenWidth, screenHeight = ScrW(), ScrH()
	local cropHeight = (bounds.uMax - bounds.uMin) * layout.width * screenHeight / screenWidth / layout.height
	local middle = (bounds.vMin + bounds.vMax) * 0.5

	surface.DrawTexturedRectUV(0, 0, screenWidth, screenHeight, bounds.uMin, middle - cropHeight * 0.5, bounds.uMax, middle + cropHeight * 0.5)
	cam.End2D()
end

local function ReportTimings()
	local timings = Testbed.timings

	if (not timings or timings.frames == 0) then return end

	if (verbose:GetBool()) then
		local stats = rcx.vr.GetStats()

		Message(string.format("%d frames in %.1f s: wait %.2f ms, eyes %.2f ms, desktop %.2f ms (means); submitted %d, skipped %d, errors %d, flush or gpu wait %.2f ms mean %.2f max, present thread %s%s",
			timings.frames, SysTime() - timings.start, timings.wait / timings.frames * 1000, timings.eyes / timings.frames * 1000, timings.desktop / timings.frames * 1000,
			stats.framesSubmitted, stats.framesSkipped, stats.submitErrors, stats.averageWaitMs, stats.maxWaitMs, stats.presentOnOtherThread and "other" or "Lua",
			stats.lastError ~= "" and (", last error: " .. stats.lastError) or ""))
	end

	Testbed.timings = {frames = 0, wait = 0, eyes = 0, desktop = 0, start = SysTime()}
end

local function Stop(reason)
	hook.Remove("RenderScene", "rcx_testbed")
	timer.Remove("rcx_testbed_report")

	-- only a session the testbed started: another addon's (Resurgence's) must survive a Lua shutdown for its hand-over
	if (not Testbed.running) then return end

	if (rcx) then
		rcx.vr.Stop()
	end

	Message("stopped" .. (reason and (": " .. reason) or ""))

	Testbed.running = false
end

local function RenderFrame()
	local waitStart = SysTime()
	local frameId, waitFailure = rcx.vr.WaitFrame()

	if (not frameId) then
		Stop("WaitFrame: " .. tostring(waitFailure))
		return
	end

	local poses = rcx.vr.GetPoses()
	local layout = Testbed.layout
	local eyesStart = SysTime()

	render.PushRenderTarget(Testbed.target)

	if (Testbed.mode == "pattern" or not poses.head.valid) then
		DrawPattern(layout, frameId)
	else
		RenderEyes(layout, poses, Testbed.display)
	end

	render.PopRenderTarget()

	local desktopStart = SysTime()
	DrawDesktop(layout)

	local submitted, submitFailure = rcx.vr.Submit(frameId)

	if (not submitted and Testbed.lastSubmitFailure ~= submitFailure) then
		Message("Submit: " .. tostring(submitFailure))
	end

	Testbed.lastSubmitFailure = submitFailure

	local timings = Testbed.timings
	timings.frames = timings.frames + 1
	timings.wait = timings.wait + (eyesStart - waitStart)
	timings.eyes = timings.eyes + (desktopStart - eyesStart)
	timings.desktop = timings.desktop + (SysTime() - desktopStart)
	return true
end

local function RenderScene()
	local succeeded, result = xpcall(RenderFrame, debug.traceback)

	if (succeeded) then
		return result
	end

	Stop("error: " .. tostring(result))
end

local function Start(mode)
	if (Testbed.running) then
		Message("already running")
		return
	end

	local loaded, loadFailure = LoadModule()

	if (not loaded) then
		Message("can't load rcx: " .. loadFailure)
		return
	end

	if (not rcx.Features().vr) then
		Message("vr is off: " .. tostring(rcx.Disabled().vr))
		return
	end

	local started, startFailure = rcx.vr.Start()

	if (not started) then
		Message("Start: " .. tostring(startFailure))
		return
	end

	local display, displayFailure = rcx.vr.GetDisplay()

	if (not display) then
		Message("GetDisplay: " .. tostring(displayFailure))
		rcx.vr.Stop()
		return
	end

	local layout = BuildLayout(display)
	local targetName = string.format("rcx_vr_eyes_%dx%d", layout.width, layout.height)

	rcx.vr.BeginTarget(layout.width, layout.height)
	local target = GetRenderTargetEx(targetName, layout.width, layout.height, RT_SIZE_LITERAL, MATERIAL_RT_DEPTH_SEPARATE, bit.bor(4, 8, 256, 512), 0, IMAGE_FORMAT_RGBA8888)
	local captured, captureFailure = rcx.vr.EndTarget()

	if (not captured) then
		Message("EndTarget: " .. tostring(captureFailure))
		rcx.vr.Stop()
		return
	end

	for _, name in ipairs({"left", "right"}) do
		local bounds = layout.bounds[name]
		rcx.vr.SetTextureBounds(name, bounds.uMin, bounds.vMin, bounds.uMax, bounds.vMax)
	end

	if (not Testbed.materials[targetName]) then
		Testbed.materials[targetName] = CreateMaterial(targetName .. "_mat", "UnlitGeneric", {["$basetexture"] = targetName})
	end

	Testbed.mode = mode or Testbed.mode
	Testbed.display = display
	Testbed.layout = layout
	Testbed.target = target
	Testbed.material = Testbed.materials[targetName]
	Testbed.originYaw = nil
	Testbed.lastSubmitFailure = nil
	Testbed.timings = {frames = 0, wait = 0, eyes = 0, desktop = 0, start = SysTime()}
	Testbed.running = true

	hook.Add("RenderScene", "rcx_testbed", RenderScene)
	timer.Create("rcx_testbed_report", 5, 0, ReportTimings)

	Message(string.format("started (%s): %.0f Hz, eye %dx%d recommended, target %dx%d, fov %.1f, aspect %.3f, mat_queue_mode %s",
		Testbed.mode, display.refreshRate, display.width, display.height, layout.width, layout.height, layout.fov, layout.aspect, GetConVar("mat_queue_mode"):GetString()))
end

concommand.Add("rcx_status", function()
	local loaded, loadFailure = LoadModule()

	if (not loaded) then
		Message("not loaded: " .. loadFailure)
		return
	end

	Message("version: " .. rcx.Version())
	Message("features: " .. JoinKeys(rcx.Features()))
	Message("disabled: " .. JoinKeys(rcx.Disabled()))

	local present, presentFailure = rcx.vr.IsHeadsetPresent()
	Message("headset: " .. (present and "present" or tostring(presentFailure)))
	PrintStats()
end)

concommand.Add("rcx_vr_test_start", function(_, _, arguments)
	local mode = arguments[1]

	if (mode and mode ~= "scene" and mode ~= "pattern") then
		Message("mode is scene or pattern")
		return
	end

	Start(mode)
end)

concommand.Add("rcx_vr_test_mode", function(_, _, arguments)
	local mode = arguments[1]

	if (mode ~= "scene" and mode ~= "pattern") then
		Message("mode is scene or pattern")
		return
	end

	Testbed.mode = mode
	Message("mode " .. mode)
end)

concommand.Add("rcx_vr_test_stop", function()
	Stop()
end)

hook.Add("ShutDown", "rcx_testbed", function()
	Stop()
end)
