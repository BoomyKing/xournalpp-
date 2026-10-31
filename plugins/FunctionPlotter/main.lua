-- FunctionPlotter — plot formulas as real Xournal++ strokes.
--
--   Plugin > Plot function...          dialog with all options
--   Plugin > Plot formula from text box   (also a toolbar button) plots what you typed in a text box
--   Plugin > Plot formula (line only)     (also a toolbar button) the same, but only the curve
--
-- Fixed and extended for Xournal++ 1.3.x (Lua 5.4/5.5, LuaGObject).
-- Based on the FunctionPlotter plugin by Roland Lötscher
-- (https://github.com/xournalpp/xournalpp/pull/3811), GPL-2.0-or-later.

local core = require("fp_core")

local SETTINGS_FILE = "settings.txt"
local saved = nil -- settings remembered between uses

-- Defaults for everything the dialog can set, used until the dialog has been used once.
local DEFAULTS = {
  formula = "y = x^2", exprX = "x = cos(t)", parametric = false, lineOnly = false,
  tMin = -3, tMax = 3, samples = 300,
  autoFit = true, xMin = -3.5, xMax = 3.5, yMin = -0.5, yMax = 10.2,
  width = 10, height = 8, unit = "cm",
  axes = true, axisLabels = true, labelX = "", labelY = "", axisColor = 0, axisWidth = 1, axisStyle = "plain",
  ticks = true, numbers = true, tickHeight = 4, xUnit = 1, yUnit = 1,
  grid = false, gridColor = 0x9a9996, gridWidth = 0.6, gridStyle = "plain",
  font = "Sans 8",
}

local UNIT_FACTORS = { pt = 1, cm = 72 / 2.54, mm = 72 / 25.4, inch = 72 }

local function dirSep() return package.config:sub(1, 1) end

local function loadGtk()
  local ok, lgi = pcall(require, "LuaGObject")
  if not ok then
    ok, lgi = pcall(require, "lgi")
  end
  if not ok then return nil end
  return lgi
end

function initUi()
  SourcePath = debug.getinfo(1).source:match("@?(.*" .. dirSep() .. ")") or ""

  -- let GTK find the toolbar icons that ship with the plugin
  local iconOk = pcall(function()
    local lgi = loadGtk()
    local Gtk = lgi.require("Gtk", "3.0")
    local theme = Gtk.IconTheme.get_default()
    theme:prepend_search_path(SourcePath .. "icons")
  end)
  local graphIcon = iconOk and "fp-plot-formula" or "xopp-draw-coordinate-system"
  local lineIcon = iconOk and "fp-plot-line" or "xopp-draw-spline"

  app.registerUi({ menu = "Plot function...", callback = "showDialog" })
  app.registerUi({ menu = "Plot formula from text box", callback = "plotFormula",
    toolbarId = "FUNCTIONPLOTTER_GRAPH", iconName = graphIcon })
  app.registerUi({ menu = "Plot formula from text box (line only)", callback = "plotFormulaLine",
    toolbarId = "FUNCTIONPLOTTER_LINE", iconName = lineIcon })
end

local function errorBox(msg)
  app.openDialog(msg, { "OK" }, "", true)
end

---------------------------------------------------------------------------
-- Remembering settings (simple key<TAB>type<TAB>value lines, no code is ever loaded)
---------------------------------------------------------------------------

local function settingsPath()
  if not app.getFolder then return nil end
  local ok, dir = pcall(app.getFolder, "config")
  if not ok or not dir or dir == "" then return nil end
  return dir .. dirSep() .. SETTINGS_FILE
end

-- Strings are stored on one line: backslash, tab and newline are escaped
local ESCAPES = { ["\\"] = "\\\\", ["\t"] = "\\t", ["\n"] = "\\n", ["\r"] = "\\r" }
local UNESCAPES = { ["\\"] = "\\", t = "\t", n = "\n", r = "\r" }
local function escape(v) return (v:gsub("[\\\t\n\r]", ESCAPES)) end
local function unescape(v) return (v:gsub("\\(.)", function(c) return UNESCAPES[c] or ("\\" .. c) end)) end

local function loadSettings()
  if saved then return saved end
  saved = {}
  for k, v in pairs(DEFAULTS) do saved[k] = v end
  local path = settingsPath()
  local f = path and io.open(path, "r")
  if not f then return saved end
  for line in f:lines() do
    local k, typ, v = line:match("^([%w_]+)\t(%a+)\t(.*)$")
    if k and DEFAULTS[k] ~= nil then
      if typ == "number" then saved[k] = tonumber(v) or saved[k]
      elseif typ == "boolean" then saved[k] = (v == "true")
      elseif typ == "string" then saved[k] = unescape(v)
      end
    end
  end
  f:close()
  return saved
end

local function storeSettings(cfg)
  saved = cfg
  local path = settingsPath()
  local f = path and io.open(path, "w")
  if not f then return end
  local keys = {}
  for k in pairs(cfg) do keys[#keys + 1] = k end
  table.sort(keys)
  for _, k in ipairs(keys) do
    local v = cfg[k]
    local typ = type(v)
    if typ == "number" or typ == "boolean" then
      f:write(k, "\t", typ, "\t", tostring(v), "\n")
    elseif typ == "string" then
      f:write(k, "\t", typ, "\t", escape(v), "\n")
    end
  end
  f:close()
end

---------------------------------------------------------------------------
-- Turning settings + a parsed formula into a graph on the page
---------------------------------------------------------------------------

-- the current page's size, so the graph lands somewhere visible on it
local function pageSize()
  local ok, doc = pcall(app.getDocumentStructure)
  if ok and doc and doc.pages and doc.currentPage and doc.pages[doc.currentPage] then
    local p = doc.pages[doc.currentPage]
    return p.pageWidth, p.pageHeight
  end
  return 595, 842
end

-- "Sans Bold 8" -> "Sans Bold", 8
local function splitFont(desc)
  local name, size = (desc or ""):match("^(.-)%s+([%d%.]+)$")
  return (name and name ~= "") and name or "Sans", tonumber(size) or 8
end

-- Builds the table fp_core.build expects. place(width, height) returns the graph's centre.
local function plotConfig(s, parsed, lineOnly, place)
  local factor = UNIT_FACTORS[s.unit] or 1
  local cfg = {}
  for k, v in pairs(s) do cfg[k] = v end
  cfg.curves, cfg.var = parsed.curves, parsed.var
  cfg.autoLabelX, cfg.autoLabelY = parsed.labelX, parsed.labelY
  cfg.tMin, cfg.tMax = core.resolveRange(parsed, s.tMin, s.tMax)
  cfg.clip = parsed.clip
  cfg.width, cfg.height = s.width * factor, s.height * factor
  cfg.fontName, cfg.fontSize = splitFont(s.font)
  if lineOnly then
    cfg.axes, cfg.ticks, cfg.numbers, cfg.grid, cfg.axisLabels = false, false, false, false, false
    cfg.autoFit = s.autoFit
  end
  cfg.cx, cfg.cy = place(cfg.width, cfg.height)
  return cfg
end

local function insertGraph(cfg)
  local result, err = core.build(cfg)
  if not result then
    errorBox(err)
    return false
  end
  local refs = app.addStrokes({ strokes = result.strokes, allowUndoRedoAction = "grouped" }) or {}
  if #result.texts > 0 then
    local trefs = app.addTexts({ texts = result.texts, allowUndoRedoAction = "grouped" }) or {}
    for _, r in ipairs(trefs) do refs[#refs + 1] = r end
  end
  app.refreshPage()
  -- select the new graph so it can be dragged into place right away
  if app.clearSelection and app.addToSelection and #refs > 0 then
    pcall(app.clearSelection)
    pcall(app.addToSelection, refs)
  end
  for _, n in ipairs(result.notes) do print("FunctionPlotter: " .. n) end
  return true
end

---------------------------------------------------------------------------
-- Toolbar buttons: plot the formula written in a text box
---------------------------------------------------------------------------

-- TeX formulas can't be read through the plugin API, but Xournal++ puts them on the clipboard when they
-- are copied. So: copy the current selection and look for TeX elements in Xournal++'s clipboard data.
-- Whether something is selected on the page: Xournal++ only enables its Copy action when there's a selection.
-- Returns true, false, or nil when it can't tell.
local function somethingSelected()
  local lgi = loadGtk()
  if not lgi then return nil end
  local result
  pcall(function()
    local Gtk = lgi.require("Gtk", "3.0")
    local Gio = lgi.Gio
    local application = Gio.Application.get_default()
    local window = application and application:get_active_window()
    if not window then
      for _, w in ipairs(Gtk.Window.list_toplevels()) do
        if w.lookup_action and w:lookup_action("copy") then window = w break end
      end
    end
    local action = window and window:lookup_action("copy")
    if action then result = action:get_enabled() end
  end)
  return result
end

local function texFromSelection()
  local lgi = loadGtk()
  if not lgi or not app.activateAction then return nil end
  local found
  pcall(function()
    local Gtk = lgi.require("Gtk", "3.0")
    local Gdk = lgi.Gdk
    local clipboard = Gtk.Clipboard.get(Gdk.Atom.intern("CLIPBOARD", false))
    local before = clipboard:wait_for_text()
    -- put a marker on the clipboard so old copied data can't be mistaken for the current selection
    clipboard:set_text("FunctionPlotter", -1)
    app.activateAction("copy")
    local data = clipboard:wait_for_contents(Gdk.Atom.intern("application/xournal", false))
    local bytes = data and data:get_data()
    if type(bytes) == "string" and #bytes > 0 then
      local list = core.readClipboardTex(bytes)
      if list and #list > 0 then found = list end
    end
    if not found then
      -- nothing useful was selected: give the clipboard back its previous text
      if before then clipboard:set_text(before, -1) else clipboard:set_text("", -1) end
    end
  end)
  if not found then return nil end
  local boxes = {}
  for _, t in ipairs(found) do
    boxes[#boxes + 1] = { text = core.fromLatex(t.text), source = t.text, x = t.x, y = t.y,
      width = t.width, height = t.height }
  end
  table.sort(boxes, function(a, b) return a.y < b.y end)
  return boxes
end

local function formulaTexts()
  -- checked first, so a button press with nothing selected never touches the clipboard
  local mayHaveTexSelected = somethingSelected() == true
  -- If a text box is still being edited, switching to the selection tool finishes the edit.
  pcall(function()
    if app.C and app.C.Tool_text and app.getActionState("select-tool") == app.C.Tool_text then
      app.changeActionState("select-tool", app.C.Tool_selectRect)
    end
  end)

  local ok, sel = pcall(app.getTexts, "selection")
  if ok and sel and #sel > 0 then
    table.sort(sel, function(a, b) return a.y < b.y end)
    return sel, true
  end
  -- a selected TeX formula
  local tex = mayHaveTexSelected and texFromSelection()
  if tex then return tex, true end
  -- nothing selected: use the newest text box on this layer that reads as a formula
  local okL, all = pcall(app.getTexts, "layer")
  if okL and all then
    for i = #all, 1, -1 do
      if core.parseFormula(all[i].text) then return { all[i] }, false end
    end
  end
  return nil
end

local function plotFromText(lineOnly)
  local boxes, selected = formulaTexts()
  if not boxes then
    errorBox("Write a formula in a text box first, for example\n\n    x = 100 - t^2, 0 <= t <= 64\n\n"
      .. "then press the button. If you have several text boxes, select the one you want with the selection tool. "
      .. "For a TeX formula, select it with the selection tool first.")
    return
  end
  local lines = {}
  local left, bottom = math.huge, -math.huge
  for _, b in ipairs(boxes) do
    lines[#lines + 1] = b.text
    left = math.min(left, b.x)
    bottom = math.max(bottom, b.y + (b.height or 0))
  end
  local parsed, err = core.parseFormula(table.concat(lines, "\n"))
  if not parsed then
    local shown = boxes[1].source and ("\n\n(TeX read as: " .. boxes[1].text .. ")")
        or (selected and "" or "\n\n(Plotted text: " .. boxes[1].text .. ")")
    errorBox(err .. shown)
    return
  end

  local s = loadSettings()
  local pageW = pageSize()
  local cfg = plotConfig(s, parsed, lineOnly, function(w, h)
    -- under the text box, leaving room for the numbers left of the axis
    local gl = math.min(left + (lineOnly and 0 or 30), pageW - w - 10)
    gl = math.max(gl, 10)
    local gt = bottom + (lineOnly and 8 or 20)
    return gl + w / 2, gt + h / 2
  end)
  cfg.labelX, cfg.labelY = "", "" -- axis names always come from the written formula
  insertGraph(cfg)
end

function plotFormula() plotFromText(false) end
function plotFormulaLine() plotFromText(true) end

---------------------------------------------------------------------------
-- Dialog
---------------------------------------------------------------------------

-- colour packing: Gdk.RGBA -> 0xRRGGBB
local function packRgba(rgba)
  local function c(v) return math.floor(v * 255 + 0.5) end
  return c(rgba.red) * 65536 + c(rgba.green) * 256 + c(rgba.blue)
end

-- "x = cos(t)" -> "cos(t)", "x(t) = cos(t)" -> "cos(t)"; "cos(t), 0 <= t <= 1" stays as it is
local function rhs(text)
  return text:match("^%s*[%a_][%w_]*%s*=%f[^=](.*)$") or text:match("^%s*[%a_][%w_]*%s*%b()%s*=%f[^=](.*)$")
      or text
end

function showDialog()
  local lgi = loadGtk()
  if not lgi then
    errorBox("The dialog needs LuaGObject (included with Xournal++ 1.3 on Windows and macOS) "
      .. "or lgi. On Linux install it with: sudo luarocks install LuaGObject\n\n"
      .. "The toolbar buttons work without it.")
    return
  end

  local Gtk = lgi.require("Gtk", "3.0")
  local Gdk = lgi.Gdk
  local builder = Gtk.Builder()
  local ok, gerr = pcall(function() return builder:add_from_file(SourcePath .. "plotter.glade") end)
  if not ok or not gerr then
    errorBox("Could not load plotter.glade from the plugin folder:\n" .. tostring(gerr))
    return
  end
  local ui = builder.objects
  local dialog = ui.dlgMain

  -- keep dependent widgets greyed out or hidden when their checkbox is off
  local function sync()
    local param = ui.cbParametric:get_active()
    ui.lblTermX:set_visible(param)
    ui.entryTermX:set_visible(param)
    ui.lblTermY:set_text(param and "y(t)" or "Formula")
    local lineOnly = ui.cbLineOnly:get_active()
    local manual = not ui.cbAutoFit:get_active()
    for _, w in ipairs({ ui.spbtXMin, ui.spbtXMax, ui.spbtYMin, ui.spbtYMax }) do w:set_sensitive(manual) end
    local axes = ui.cbAxisLines:get_active() and not lineOnly
    ui.cbAxisLines:set_sensitive(not lineOnly)
    ui.colAxis:set_sensitive(axes)
    ui.comboAxis:set_sensitive(axes)
    ui.spbtLineWidthAxis:set_sensitive(axes)
    ui.cbAxisLabels:set_sensitive(axes)
    ui.entryLabelX:set_sensitive(axes)
    ui.entryLabelY:set_sensitive(axes)
    local grid = ui.cbGrid:get_active() and not lineOnly
    ui.cbGrid:set_sensitive(not lineOnly)
    ui.colGrid:set_sensitive(grid)
    ui.comboGrid:set_sensitive(grid)
    ui.spbtLineWidthGrid:set_sensitive(grid)
    ui.cbTicks:set_sensitive(not lineOnly)
    ui.cbNumbers:set_sensitive(not lineOnly)
    local ticks = not lineOnly and (ui.cbTicks:get_active() or grid or ui.cbNumbers:get_active())
    ui.spbtXUnit:set_sensitive(ticks)
    ui.spbtYUnit:set_sensitive(ticks)
    ui.spbtTickHeight:set_sensitive(not lineOnly and ui.cbTicks:get_active())
  end

  -- widget <-> settings mapping
  local spins = { tMin = "spbtTMin", tMax = "spbtTMax", samples = "spbtSamples", xMin = "spbtXMin", xMax = "spbtXMax",
    yMin = "spbtYMin", yMax = "spbtYMax", xUnit = "spbtXUnit", yUnit = "spbtYUnit", width = "spbtWidth",
    height = "spbtHeight", axisWidth = "spbtLineWidthAxis", gridWidth = "spbtLineWidthGrid", tickHeight = "spbtTickHeight" }
  local entries = { formula = "entryTermY", exprX = "entryTermX", labelX = "entryLabelX", labelY = "entryLabelY",
    unit = "comboentryUnit", axisStyle = "comboentryAxis", gridStyle = "comboentryGrid" }
  local checks = { autoFit = "cbAutoFit", axes = "cbAxisLines", axisLabels = "cbAxisLabels", ticks = "cbTicks",
    numbers = "cbNumbers", grid = "cbGrid", parametric = "cbParametric", lineOnly = "cbLineOnly" }
  local colours = { axisColor = "colAxis", gridColor = "colGrid" }

  local function restore(s)
    for k, id in pairs(spins) do if type(s[k]) == "number" then pcall(function() ui[id]:set_value(s[k]) end) end end
    for k, id in pairs(entries) do if type(s[k]) == "string" then pcall(function() ui[id]:set_text(s[k]) end) end end
    for k, id in pairs(checks) do if type(s[k]) == "boolean" then pcall(function() ui[id]:set_active(s[k]) end) end end
    for k, id in pairs(colours) do
      if type(s[k]) == "number" then
        pcall(function()
          local v = math.floor(s[k])
          ui[id]:set_rgba(Gdk.RGBA({ red = (v // 65536 % 256) / 255, green = (v // 256 % 256) / 255,
            blue = (v % 256) / 255, alpha = 1 }))
        end)
      end
    end
    if type(s.font) == "string" then pcall(function() ui.fnt:set_font(s.font) end) end
  end

  local function read()
    local s = {}
    for k, id in pairs(spins) do s[k] = ui[id]:get_value() end
    for k, id in pairs(entries) do s[k] = ui[id]:get_text() end
    for k, id in pairs(checks) do s[k] = ui[id]:get_active() end
    for k, id in pairs(colours) do s[k] = packRgba(ui[id]:get_rgba()) end
    s.font = ui.fnt:get_font() or "Sans 8"
    return s
  end

  restore(loadSettings())
  sync()

  for _, id in ipairs({ "cbAutoFit", "cbAxisLines", "cbGrid", "cbTicks", "cbNumbers", "cbParametric", "cbLineOnly" }) do
    ui[id].on_toggled = function() sync() end
  end

  function ui.btnDiscard.on_clicked()
    dialog:destroy()
  end

  function ui.btnAddGraph.on_clicked()
    local okRun, runErr = pcall(function()
      local s = read()
      local parsed, err
      if s.parametric then
        parsed, err = core.parseFormula("x = " .. rhs(s.exprX) .. "\ny = " .. rhs(s.formula))
        if parsed and not parsed.parametric then
          -- e.g. both parts constant: fall back to plain parametric in t
          parsed = { curves = { { exprX = rhs(s.exprX), exprY = rhs(s.formula) } }, var = "t",
            labelX = "x", labelY = "y" }
        end
      else
        parsed, err = core.parseFormula(s.formula)
      end
      if not parsed then
        errorBox(err)
        return
      end
      local pageW, pageH = pageSize()
      local cfg = plotConfig(s, parsed, s.lineOnly, function() return pageW / 2, pageH / 2 end)
      if insertGraph(cfg) then
        storeSettings(s)
        dialog:destroy()
      end
    end)
    if not okRun then
      errorBox("Something went wrong while plotting:\n" .. tostring(runErr))
    end
  end

  dialog:show_all()
  sync() -- show_all makes the parametric row visible again
end
