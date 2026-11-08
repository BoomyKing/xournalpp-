-- fp_core.lua — the math behind the FunctionPlotter plugin for Xournal++.
--
-- Pure Lua (5.3 / 5.4 / 5.5), no GTK and no Xournal++ API, so it can be tested on its own.
-- It turns the dialog settings into the stroke and text tables that app.addStrokes and
-- app.addTexts expect.
--
-- Based on the FunctionPlotter plugin by Roland Lötscher
-- (https://github.com/xournalpp/xournalpp/pull/3811), GPL-2.0-or-later.

local M = {}

local floor, ceil, abs, huge = math.floor, math.ceil, math.abs, math.huge

---------------------------------------------------------------------------
-- Expressions
---------------------------------------------------------------------------

local function sign(v) return v > 0 and 1 or (v < 0 and -1 or 0) end
local function ln(v) return math.log(v) end
local function log10(v) return math.log(v, 10) end
local function log2(v) return math.log(v, 2) end
local function round(v) return floor(v + 0.5) end
local function sinh(v) return (math.exp(v) - math.exp(-v)) / 2 end
local function cosh(v) return (math.exp(v) + math.exp(-v)) / 2 end
local function tanh(v) local a, b = math.exp(v), math.exp(-v); return (a - b) / (a + b) end
local function atan(y, x) if x == nil then return math.atan(y) end return math.atan(y, x) end
local function root(v, n) -- real n-th root, so root(-8, 3) = -2
  if v < 0 and n % 2 == 1 then return -((-v) ^ (1 / n)) end
  return v ^ (1 / n)
end

-- Everything an expression may use. Nothing else is reachable from inside an expression.
local FUNCTIONS = {
  sin = math.sin, cos = math.cos, tan = math.tan,
  asin = math.asin, acos = math.acos, atan = atan,
  arcsin = math.asin, arccos = math.acos, arctan = atan,
  sinh = sinh, cosh = cosh, tanh = tanh,
  sqrt = math.sqrt, root = root, cbrt = function(v) return root(v, 3) end,
  abs = abs, exp = math.exp, ln = ln, log = log10, log2 = log2,
  floor = floor, ceil = ceil, round = round, sign = sign,
  min = math.min, max = math.max,
}
local CONSTANTS = { pi = math.pi, e = math.exp(1) }
local VARIABLES = { t = true, x = true }

M.FUNCTION_NAMES = {}
for k in pairs(FUNCTIONS) do M.FUNCTION_NAMES[#M.FUNCTION_NAMES + 1] = k end
table.sort(M.FUNCTION_NAMES)

-- "pix" -> pi x, "ex" -> e x, "tpi" -> t pi: a name glued together from constants and one single-letter
-- variable (as in 2πx). Returns the pieces, or nil when the name isn't such a combination.
local function splitGluedName(id)
  if FUNCTIONS[id] or CONSTANTS[id] or id:match("^%a[%d_]*$") then return nil end
  local pieces, letters, i = {}, 0, 1
  while i <= #id do
    local c = id:match("^pi", i) or id:match("^e", i)
    if c then
      pieces[#pieces + 1] = c
    else
      c = id:match("^%a", i)
      if not c then return nil end
      letters = letters + 1
      pieces[#pieces + 1] = c
    end
    i = i + #c
  end
  if letters > 1 or letters == #pieces then return nil end
  return pieces
end

-- Splits an expression into tokens. Returns a list or nil, error message.
local function tokenize(expr)
  local tokens, i, n = {}, 1, #expr
  while i <= n do
    local c = expr:sub(i, i)
    if c:match("%s") then
      i = i + 1
    else
      local num = expr:match("^%d+%.?%d*[eE][%+%-]?%d+", i) or expr:match("^%.%d+[eE][%+%-]?%d+", i)
          or expr:match("^%d+%.?%d*", i) or expr:match("^%.%d+", i)
      if num then
        tokens[#tokens + 1] = { kind = "num", text = num }
        i = i + #num
      else
        local id = expr:match("^[%a_][%w_]*", i)
        if id then
          local glued = splitGluedName(id)
          if glued then
            for _, piece in ipairs(glued) do tokens[#tokens + 1] = { kind = "id", text = piece } end
          else
            tokens[#tokens + 1] = { kind = "id", text = id }
          end
          i = i + #id
        elseif expr:sub(i, i + 1) == "**" then
          tokens[#tokens + 1] = { kind = "op", text = "^" }
          i = i + 2
        elseif c == "|" then
          tokens[#tokens + 1] = { kind = "bar", text = "|" }
          i = i + 1
        elseif c:match("[%+%-%*/%^%%%(%),]") then
          tokens[#tokens + 1] = { kind = c == "(" and "lp" or (c == ")" and "rp" or "op"), text = c }
          i = i + 1
        else
          return nil, string.format("Unexpected character '%s'.", c)
        end
      end
    end
  end
  return tokens
end

local LUA_KEYWORDS = {}
for kw in ("and break do else elseif end false for function goto if in local nil not or repeat return then true until while global"):gmatch("%a+") do
  LUA_KEYWORDS[kw] = true
end

-- Turns |...| into abs(...). A bar opens when it can't end a value (start, after an operator, "(" or ","),
-- otherwise it closes the innermost open bar; with none open, it opens one (as in 2|x|).
local function resolveBars(tokens)
  local out, open, prev = {}, 0, nil
  for _, tok in ipairs(tokens) do
    if tok.kind == "bar" then
      local endsValue = prev and (prev.kind == "num" or prev.kind == "id" or prev.kind == "rp")
      if endsValue and open > 0 then
        out[#out + 1] = { kind = "rp", text = ")" }
        open = open - 1
        prev = out[#out]
      else
        out[#out + 1] = { kind = "id", text = "abs" }
        out[#out + 1] = { kind = "lp", text = "(" }
        open = open + 1
        prev = out[#out]
      end
    else
      out[#out + 1] = tok
      prev = tok
    end
  end
  if open > 0 then return nil, "A | is missing: absolute values are written like |x - 2|." end
  return out
end

--- True if name can be used as the variable of a formula.
function M.validVariable(name)
  return type(name) == "string" and name:match("^[%a_][%w_]*$") ~= nil
      and not FUNCTIONS[name] and not CONSTANTS[name] and not LUA_KEYWORDS[name]
end

--- The first name used like a function (followed by a bracket) that isn't one, e.g. "sec" in "sec(x)", or nil.
--- Single letters such as t in t(t + 1) are variables, multiplied with the bracket.
function M.unknownFunction(expr)
  local tokens = tokenize(expr or "") or {}
  for i, tok in ipairs(tokens) do
    local nxt = tokens[i + 1]
    if tok.kind == "id" and nxt and nxt.kind == "lp" and not FUNCTIONS[tok.text] and not CONSTANTS[tok.text]
        and not tok.text:match("^%a[%d_]*$") then
      return tok.text
    end
  end
  return nil
end

--- The variable-like names used in an expression (anything that isn't a function or constant).
function M.names(expr)
  local tokens = tokenize(expr or "")
  local set, list = {}, {}
  for _, tok in ipairs(tokens or {}) do
    if tok.kind == "id" and not FUNCTIONS[tok.text] and not CONSTANTS[tok.text] and not set[tok.text] then
      set[tok.text] = true
      list[#list + 1] = tok.text
    end
  end
  return list
end

--- Compiles an expression in one variable.
--- var is the variable's name; when it is nil, both t and x are accepted (as the same variable).
--- Returns a function f(value) -> number or nil (nil where the expression is undefined),
--- or nil plus an error message meant for the user.
function M.compile(expr, var)
  if type(expr) ~= "string" or expr:match("^%s*$") then
    return nil, "The expression is empty."
  end
  if var ~= nil and not M.validVariable(var) then
    return nil, string.format("'%s' can't be used as a variable name.", tostring(var))
  end
  local allowed = var and { [var] = true } or VARIABLES
  local tokens, err = tokenize(expr)
  if not tokens then return nil, err end
  tokens, err = resolveBars(tokens)
  if not tokens then return nil, err end

  local out, prev = {}, nil
  for idx, tok in ipairs(tokens) do
    if tok.kind == "id" then
      local name = tok.text
      if not (FUNCTIONS[name] or CONSTANTS[name] or allowed[name]) then
        return nil, string.format("Unknown name '%s'. The variable here is %s.", name, var or "t")
      end
      if FUNCTIONS[name] then
        local nxt = tokens[idx + 1]
        if not nxt or nxt.kind ~= "lp" then
          return nil, string.format("%s needs brackets, e.g. %s(%s).", name, name, var or "t")
        end
      end
    end
    -- implicit multiplication: 2t, 3(t+1), (t+1)(t-1), 2pi, t sin(t)
    if prev then
      local prevValue = prev.kind == "num" or prev.kind == "rp"
          or (prev.kind == "id" and not FUNCTIONS[prev.text])
      local curValue = tok.kind == "num" or tok.kind == "id" or tok.kind == "lp"
      if prevValue and curValue then
        if prev.kind == "num" and tok.kind == "num" then
          return nil, string.format("Missing operator between %s and %s.", prev.text, tok.text)
        end
        out[#out + 1] = "*"
      end
    end
    local text = tok.text
    if tok.kind == "num" and not text:find("[%.eE]") then
      text = text .. ".0" -- keep all arithmetic in floating point
    end
    out[#out + 1] = text
    prev = tok
  end

  local env = {}
  for k, v in pairs(FUNCTIONS) do env[k] = v end
  for k, v in pairs(CONSTANTS) do env[k] = v end
  local head = var and ("local " .. var .. " = ...; ") or "local t = ...; local x = t; "
  local src = head .. "return (" .. table.concat(out, " ") .. ")"
  local chunk = load(src, "=expression", "t", env)
  if not chunk then
    return nil, "Could not read the expression. Check for a missing number, operator or bracket."
  end

  return function(t)
    local ok, v = pcall(chunk, t)
    if ok and type(v) == "number" and v == v and v ~= huge and v ~= -huge then
      return v
    end
    return nil
  end
end

--- Evaluates a constant expression such as "2pi" or "-3.5". Returns a number or nil.
function M.constant(expr)
  local f = M.compile(expr, "_unused_")
  local v = f and f(0)
  return v
end

---------------------------------------------------------------------------
-- Reading a written formula, e.g. "y = 100 - t^2, 0 <= t <= 64"
---------------------------------------------------------------------------

-- Turns common typed/pasted maths symbols into plain ASCII.
local REPLACEMENTS = {
  { "−", "-" }, { "–", "-" }, { "—", "-" }, { "·", "*" }, { "⋅", "*" }, { "×", "*" }, { "÷", "/" },
  { "²", "^2" }, { "³", "^3" }, { "π", "pi" }, { "≤", "<=" }, { "≥", ">=" }, { "⩽", "<=" }, { "⩾", ">=" },
  { "∈", " in " }, { "\u{00A0}", " " },
}
function M.normalize(text)
  for _, r in ipairs(REPLACEMENTS) do
    text = text:gsub(r[1], r[2])
  end
  text = text:gsub("√(%b())", "sqrt%1")
  text = text:gsub("√%s*([%w%.]+)", "sqrt(%1)")
  return text
end

local function trim(s) return (s:gsub("^%s+", ""):gsub("%s+$", "")) end

-- Splits on , ; newlines { } and the words for/where/when/with/if, but not inside brackets.
local function splitParts(text)
  local parts, buf, depth = {}, {}, 0
  local i, n = 1, #text
  local function flush()
    local p = trim(table.concat(buf))
    if p ~= "" then parts[#parts + 1] = p end
    buf = {}
  end
  while i <= n do
    local c = text:sub(i, i)
    local word = text:match("^[%a_][%w_]*", i)
    if word then
      local before = i == 1 and " " or text:sub(i - 1, i - 1)
      if (word == "for" or word == "where" or word == "when" or word == "with" or word == "if")
          and not before:match("[%w_]") then
        flush()
      else
        buf[#buf + 1] = word
      end
      i = i + #word
    else
      if c == "(" or c == "[" then depth = depth + 1 end
      if c == ")" or c == "]" then depth = math.max(0, depth - 1) end
      if depth == 0 and (c == "," or c == ";" or c == "\n" or c == "\r" or c == "{" or c == "}") then
        flush()
      else
        buf[#buf + 1] = c
      end
      i = i + 1
    end
  end
  flush()
  return parts
end

local function isCondition(part)
  return part:find("[<>]") ~= nil or part:match("^[%a_][%w_]*%s+in%s") ~= nil
end

local function isVarName(p) return M.validVariable(p) end

-- "steps on x = 1", "step x = 0.5", "step = 2", "steps 1", "Δx = 1": draw the graph only at every step of the
-- variable, as dots joined by straight lines. Returns the text of the step size and the variable (or nil), or
-- nil when the part isn't about steps.
local function stepParts(part)
  part = trim(part)
  local v, size = part:match("^[Ss]teps?%s+on%s+([%a_][%w_]*)%s*[=:]?%s*(.+)$")
  if not v then v, size = part:match("^[Ss]teps?%s+([%a_][%w_]*)%s*[=:]%s*(.+)$") end
  if not v then v, size = part:match("^\u{0394}%s*([%a_][%w_]*)%s*=%s*(.+)$") end
  if not v then size = part:match("^[Ss]teps?%s*[=:]?%s*([^%a].*)$") end
  return size, v
end
local function isStep(part) return stepParts(part) ~= nil end

-- One condition, e.g. "0 <= t <= 64", "64 >= t >= 0", "y > 0", "x <= 3", "2 < x", "t in [0, 64]".
-- Returns name, lo, hi (lo or hi may be nil) or nil, error.
local function parseCondition(part)
  local bad = "Couldn't read '" .. part .. "'. Write conditions like  0 <= t <= 64  or  y > 0."
  local pieces, ops = {}, {}
  local v, a, b = part:match("^%s*([%a_][%w_]*)%s+in%s*[%[%(](.-),(.-)[%]%)]%s*$")
  if v then
    pieces, ops = { a, v, b }, { "<", "<" }
  else
    local pos = 1
    while true do
      local s1, e1 = part:find("[<>]=?", pos)
      if not s1 then break end
      pieces[#pieces + 1] = part:sub(pos, s1 - 1)
      ops[#ops + 1] = part:sub(s1, s1)
      pos = e1 + 1
    end
    pieces[#pieces + 1] = part:sub(pos)
  end
  for i, p in ipairs(pieces) do pieces[i] = trim(p) end

  -- absolute values: |v| < c  ->  -c < v < c,   |v - a| < c  ->  a - c < v < a + c
  local function absPart(p)
    local inner = p:match("^|%s*(.-)%s*|$")
    if not inner then return nil end
    local name = inner:match("^([%a_][%w_]*)$")
    local shift = 0
    if not name then
      local nm, sign, rest = inner:match("^([%a_][%w_]*)%s*([%+%-])%s*(.+)$")
      local c = nm and M.constant(rest)
      if not c then return nil end
      name, shift = nm, (sign == "-") and c or -c
    end
    return name, shift
  end
  if #ops == 1 then
    local absName, shift, bound, op
    local n1, s1 = absPart(pieces[1])
    local n2, s2 = absPart(pieces[2])
    if n1 then
      absName, shift, bound, op = n1, s1, pieces[2], ops[1]
    elseif n2 then
      absName, shift, bound, op = n2, s2, pieces[1], (ops[1] == "<") and ">" or "<"
    end
    if absName then
      local c = M.constant(bound)
      if not c then return nil, "'" .. bound .. "' in '" .. part .. "' should be a number." end
      if op == ">" then
        return nil, "'" .. part .. "' would split the graph in two pieces, which isn't supported. " ..
            "Write it as two formulas or use a range instead."
      end
      if c <= 0 then return nil, "'" .. part .. "' leaves nothing to draw." end
      return absName, shift - c, shift + c
    end
  end

  local function num(p)
    local val = M.constant(p)
    if not val then return nil, "'" .. p .. "' in '" .. part .. "' should be a number." end
    return val
  end

  local name, lo, hi
  if #ops == 1 then
    local left, right, op = pieces[1], pieces[2], ops[1]
    if isVarName(left) and not isVarName(right) then
      local c, e = num(right); if not c then return nil, e end
      name = left
      if op == "<" then hi = c else lo = c end
    elseif isVarName(right) and not isVarName(left) then
      local c, e = num(left); if not c then return nil, e end
      name = right
      if op == "<" then lo = c else hi = c end
    else
      return nil, bad
    end
  elseif #ops == 2 then
    if ops[1] ~= ops[2] or not isVarName(pieces[2]) then return nil, bad end
    local c1, e1 = num(pieces[1]); if not c1 then return nil, e1 end
    local c3, e3 = num(pieces[3]); if not c3 then return nil, e3 end
    name = pieces[2]
    if ops[1] == "<" then lo, hi = c1, c3 else lo, hi = c3, c1 end
  else
    return nil, bad
  end
  if lo and hi and lo >= hi then
    return nil, "In '" .. part .. "' the smaller number must come first."
  end
  return name, lo, hi
end

--- Reads a written formula. Accepts things like
---   100 - t^2                      y = x^2 - 3x, -2 <= x <= 5
---   x = 100 - t^2, 0 <= t <= 64     f(x) = sin(x) for 0 <= x <= 2pi
---   y = 100 - x^2, 0 <= x <= 64, y > 0     (extra conditions cut the curve)
---   x = cos(t), y = sin(t), 0 <= t <= 2pi  (parametric)
---   y = x^2; y = 2x + 1             (several functions on the same axes)
--- Returns { curves = {{exprX, exprY}...}, var, labelX, labelY, tMin, tMax, clip = {xMin, xMax, yMin, yMax} }
--- (tMin/tMax may be nil, clip values may be nil), or nil plus an error message.
local parseOne

-- Splits into lines at new lines and semicolons that aren't inside brackets.
local function splitLines(text)
  local lines, depth, start = {}, 0, 1
  for i = 1, #text do
    local c = text:sub(i, i)
    if c == "(" or c == "[" then depth = depth + 1
    elseif c == ")" or c == "]" then depth = math.max(0, depth - 1)
    elseif depth == 0 and (c == "\n" or c == ";") then
      lines[#lines + 1] = text:sub(start, i - 1)
      start = i + 1
    end
  end
  lines[#lines + 1] = text:sub(start)
  local out = {}
  for _, l in ipairs(lines) do if trim(l) ~= "" then out[#out + 1] = l end end
  return out
end

-- v(0 <= t <= 3) = t  ->  v(t) = t, 0 <= t <= 3: a range written inside the brackets of the name.
-- Each such formula becomes a line of its own (a piece), keeping the other parts of its line.
local function liftRangesFromNames(text)
  local out = {}
  for _, line in ipairs(splitLines(text)) do
    local rest, lifted = {}, {}
    for _, part in ipairs(splitParts(line)) do
      local name, inner, rhs = part:match("^%s*([%a_][%w_]*)%s*(%b())%s*=(.*)$")
      local cond = inner and trim(inner:sub(2, -2))
      local var = cond and isCondition(cond) and not rhs:find("^=") and parseCondition(cond)
      if var then
        lifted[#lifted + 1] = name .. "(" .. var .. ") = " .. rhs .. ", " .. cond
      else
        rest[#rest + 1] = part
      end
    end
    if #lifted == 0 then
      out[#out + 1] = line
    elseif #lifted == 1 then
      out[#out + 1] = table.concat({ lifted[1], table.unpack(rest) }, ", ")
    else
      for _, l in ipairs(lifted) do out[#out + 1] = l end
      if #rest > 0 then out[#out + 1] = table.concat(rest, ", ") end
    end
  end
  return table.concat(out, "\n")
end

function M.parseFormula(text)
  if type(text) ~= "string" or trim(text) == "" then
    return nil, "There's no formula. Write something like  y = 100 - t^2, 0 <= t <= 64"
  end
  text = liftRangesFromNames(M.normalize(text))

  -- Piecewise: several lines that each have a formula and their own range, e.g.
  --   0 <= t <= 0.6, v(t) = 3t
  --   0.6 <= t <= 2, v(t) = 1.8
  -- Lines with only conditions apply to every piece. Without at least two such lines, all conditions apply to all
  -- formulas (y = x^2; y = 2x + 1; -2 <= x <= 2).
  local formulaLines, condLines, withRange = {}, {}, 0
  for _, line in ipairs(splitLines(text)) do
    local hasFormula, hasCond = false, false
    for _, part in ipairs(splitParts(line)) do
      if isCondition(part) or isStep(part) then hasCond = true else hasFormula = true end
    end
    if hasFormula then
      formulaLines[#formulaLines + 1] = line
      if hasCond then withRange = withRange + 1 end
    else
      condLines[#condLines + 1] = line
    end
  end
  if withRange < 2 then return parseOne(text) end

  local shared = table.concat(condLines, "\n")
  local result
  for i, line in ipairs(formulaLines) do
    local piece, err = parseOne(line .. "\n" .. shared)
    if not piece then
      return nil, (#formulaLines > 1 and ("Line " .. i .. ": ") or "") .. err
    end
    if not result then
      result = { curves = {}, var = piece.var, parametric = piece.parametric, labelX = piece.labelX,
        labelY = piece.labelY, tMin = piece.tMin, tMax = piece.tMax, clip = {} }
    elseif piece.var ~= result.var then
      return nil, string.format("The lines use different variables (%s and %s).", result.var, piece.var)
    else
      if piece.labelY ~= result.labelY then result.labelY = "y" end
      if piece.labelX ~= result.labelX then result.labelX = result.var end
      result.tMin = (result.tMin and piece.tMin) and math.min(result.tMin, piece.tMin) or nil
      result.tMax = (result.tMax and piece.tMax) and math.max(result.tMax, piece.tMax) or nil
    end
    for _, c in ipairs(piece.curves) do
      -- pieces with the same name (or no name) are parts of one function
      c.group = piece.parametric and "(parametric)" or (c.name or "(unnamed)")
      c.name = c.name or (not piece.parametric and piece.labelY) or nil
      c.tMin, c.tMax, c.clip = piece.tMin, piece.tMax, piece.clip
      result.curves[#result.curves + 1] = c
    end
  end
  return result
end

--- Reads one formula (or several sharing the same conditions).
function parseOne(text)

  local formulas, conds, condOrder = {}, {}, {}
  local step, stepVar
  for _, part in ipairs(splitParts(text)) do
    if isStep(part) then
      local size, v = stepParts(part)
      local value = M.constant(size)
      if not (value and value > 0 and value < math.huge) then
        return nil, "Couldn't read the step size in '" .. trim(part) .. "'. Write it like  steps on x = 1"
      end
      step, stepVar = value, v
    elseif isCondition(part) then
      local name, lo, hi = parseCondition(part)
      if not name then return nil, lo end
      local c = conds[name]
      if not c then
        c = {}
        conds[name] = c
        condOrder[#condOrder + 1] = name
      end
      if lo then c.lo = c.lo and math.max(c.lo, lo) or lo end
      if hi then c.hi = c.hi and math.min(c.hi, hi) or hi end
      if c.lo and c.hi and c.lo >= c.hi then
        return nil, "The conditions on " .. name .. " leave nothing to draw."
      end
    else
      local lhs, rhs = part:match("^(.-)=(.*)$")
      if lhs then
        local name, arg = trim(lhs):match("^([%a_][%w_]*)%s*%(%s*([%a_][%w_]*)%s*%)$")
        name = name or trim(lhs):match("^([%a_][%w_]*)$")
        if not name then
          return nil, "Couldn't read '" .. trim(lhs) .. " ='. Write it like  y = x^2  or  f(x) = x^2."
        end
        if rhs:find("=") then return nil, "Too many = signs in '" .. part .. "'." end
        formulas[#formulas + 1] = { name = name, arg = arg, expr = trim(rhs) }
      else
        formulas[#formulas + 1] = { expr = part }
      end
    end
  end
  if #formulas == 0 then
    return nil, "There are only conditions, no formula. Write something like  y = 100 - t^2, 0 <= t <= 64"
  end
  for _, f in ipairs(formulas) do
    local unknown = M.unknownFunction(f.expr)
    if unknown then
      return nil, string.format("Unknown function '%s'. The functions are: %s.", unknown,
        table.concat(M.FUNCTION_NAMES, ", "))
    end
  end

  local lhsNames = {}
  for _, f in ipairs(formulas) do if f.name then lhsNames[f.name] = true end end
  -- condition names that aren't a formula's left side: candidates for the variable
  local condVars = {}
  for _, nm in ipairs(condOrder) do if not lhsNames[nm] then condVars[#condVars + 1] = nm end end

  local function range(var)
    local c = conds[var]
    return c and c.lo, c and c.hi
  end
  local function clipOf(name)
    local c = conds[name]
    return c and c.lo, c and c.hi
  end
  local function checkConds(allowed)
    for _, nm in ipairs(condOrder) do
      if not allowed[nm] then
        local list = {}
        for k in pairs(allowed) do list[#list + 1] = k end
        table.sort(list)
        return nil, string.format("There's a condition on %s, but the formula only has %s.", nm,
          table.concat(list, " and "))
      end
    end
    return true
  end

  -- parametric: x = ..., y = ... (or x(t) = ..., y(t) = ...) both in a third variable.
  -- Other names, like f(t) = ... and g(t) = ..., are two separate graphs.
  local n1, n2 = formulas[1].name, formulas[2] and formulas[2].name
  if #formulas == 2 and ((n1 == "x" and n2 == "y") or (n1 == "y" and n2 == "x")) then
    local a, b = formulas[1], formulas[2]
    if a.name == "y" and b.name == "x" then a, b = b, a end
    local used = {}
    for _, f in ipairs({ a, b }) do
      for _, nm in ipairs(M.names(f.expr)) do used[nm] = true end
    end
    local var = condVars[1] or a.arg or b.arg
    if not var then
      local list = {}
      for nm in pairs(used) do list[#list + 1] = nm end
      if #list == 1 then var = list[1] elseif used.t then var = "t" end
    end
    if var and var ~= a.name and var ~= b.name and not used[a.name] and not used[b.name] then
      local ok, err = checkConds({ [var] = true, [a.name] = true, [b.name] = true })
      if not ok then return nil, err end
      local tMin, tMax = range(var)
      local xLo, xHi = clipOf(a.name)
      local yLo, yHi = clipOf(b.name)
      if stepVar and stepVar ~= var then
        return nil, string.format("The steps are on %s, but the curve's variable is %s.", stepVar, var)
      end
      return { curves = { { exprX = a.expr, exprY = b.expr, step = step } }, var = var, parametric = true,
        labelX = a.name, labelY = b.name, tMin = tMin, tMax = tMax,
        clip = { xMin = xLo, xMax = xHi, yMin = yLo, yMax = yHi } }
    end
  end

  -- ordinary function(s) of one variable
  local var
  for _, f in ipairs(formulas) do
    if f.arg then
      if var and var ~= f.arg then
        return nil, string.format("The formulas use different variables (%s and %s).", var, f.arg)
      end
      var = f.arg
    end
  end
  if not var and #condVars == 1 then
    local exprNames = {}
    for _, f in ipairs(formulas) do
      for _, nm in ipairs(M.names(f.expr)) do exprNames[nm] = true end
    end
    if next(exprNames) and not exprNames[condVars[1]] then
      local list = {}
      for k in pairs(exprNames) do list[#list + 1] = k end
      table.sort(list)
      return nil, string.format("There's a condition on %s, but the formula uses %s.", condVars[1],
        table.concat(list, " and "))
    end
    var = condVars[1]
  end
  if not var then
    local seen, list = {}, {}
    for _, f in ipairs(formulas) do
      for _, nm in ipairs(M.names(f.expr)) do
        if not seen[nm] then seen[nm] = true; list[#list + 1] = nm end
      end
    end
    if #list > 1 then
      return nil, "The formula uses more than one variable (" .. table.concat(list, ", ") .. ")."
    end
    var = list[1] or condVars[1] or "x"
  end
  if not M.validVariable(var) then
    return nil, "'" .. var .. "' can't be used as a variable name."
  end
  if not var:match("^%a[%d_]*$") then
    return nil, "'" .. var .. "' isn't a formula variable. Use a single letter like t or x."
  end
  local curves, depName = {}, nil
  for _, f in ipairs(formulas) do
    for _, nm in ipairs(M.names(f.expr)) do
      if nm ~= var then
        return nil, string.format("The formula uses %s, but the variable is %s.", nm, var)
      end
    end
    if f.name == var then
      return nil, string.format("'%s' can't be on both sides of the formula.", var)
    end
    depName = depName or f.name
    curves[#curves + 1] = { exprX = var, exprY = f.expr, name = f.name }
  end
  -- several functions with different names (v and x) share the vertical axis: call it y
  local differentNames = false
  for _, f in ipairs(formulas) do
    if f.name and f.name ~= depName then differentNames = true end
  end
  if not depName or (#formulas > 1 and (depName:match("^f") or differentNames)) then
    depName = (var == "y") and "f" or "y"
  end
  -- conditions on the result (y > 0) cut the curve; "y" always means the vertical value
  local allowed = { [var] = true, [depName] = true }
  for nm in pairs(lhsNames) do allowed[nm] = true end
  if var ~= "y" then allowed.y = true end
  local ok, err = checkConds(allowed)
  if not ok then return nil, err end
  local yLo, yHi
  for _, nm in ipairs(condOrder) do
    if nm ~= var then
      local lo, hi = clipOf(nm)
      if lo then yLo = yLo and math.max(yLo, lo) or lo end
      if hi then yHi = yHi and math.min(yHi, hi) or hi end
    end
  end
  if yLo and yHi and yLo >= yHi then
    return nil, "The conditions on " .. depName .. " leave nothing to draw."
  end
  if stepVar and stepVar ~= var then
    return nil, string.format("The steps are on %s, but the graph's variable is %s.", stepVar, var)
  end
  for _, c in ipairs(curves) do c.step = step end
  local tMin, tMax = range(var)
  return { curves = curves, var = var, parametric = false,
    labelX = var, labelY = depName, tMin = tMin, tMax = tMax,
    clip = { yMin = yLo, yMax = yHi } }
end

--- Fills in a missing end of the range from the defaults (e.g. "x >= 0" with default -3..3 -> 0..3).
function M.resolveRange(parsed, defLo, defHi)
  local lo, hi = parsed.tMin or defLo, parsed.tMax or defHi
  if lo >= hi then
    local width = math.max(defHi - defLo, 1)
    if parsed.tMin and not parsed.tMax then hi = lo + width
    elseif parsed.tMax and not parsed.tMin then lo = hi - width end
  end
  return lo, hi
end

---------------------------------------------------------------------------
-- LaTeX (TeX tool) formulas -> plain formulas
---------------------------------------------------------------------------

local TEX_SYMBOLS = {
  cdot = "*", times = "*", div = "/", ast = "*", lvert = "|", rvert = "|", vert = "|", lVert = "|", rVert = "|",
  le = "<=", leq = "<=", leqslant = "<=", ge = ">=", geq = ">=", geqslant = ">=", lt = "<", gt = ">",
  pi = "pi", ["in"] = " in ", left = "", right = "", displaystyle = "", textstyle = "", big = "", Big = "",
  quad = " ", qquad = " ", [","] = " ", [";"] = " ", [":"] = " ", ["!"] = "", [" "] = " ",
  ["{"] = " { ", ["}"] = " } ", ["%"] = "%", cdots = "", ldots = "", mid = " , ",
  colon = ":", land = ",", wedge = ",", text = "", mathrm = "", mathit = "", operatorname = "",
}
local TEX_FUNCTIONS = {
  sin = true, cos = true, tan = true, arcsin = true, arccos = true, arctan = true, sinh = true, cosh = true,
  tanh = true, ln = true, log = true, exp = true, min = true, max = true, sqrt = true,
}

-- returns the text of a {...} group starting at i (s:sub(i,i) == "{") and the index after it
local function braceGroup(s, i)
  local depth = 0
  for j = i, #s do
    local c = s:sub(j, j)
    if c == "\\" then
      -- skip escaped characters such as \{
    elseif c == "{" and s:sub(j - 1, j - 1) ~= "\\" then depth = depth + 1
    elseif c == "}" and s:sub(j - 1, j - 1) ~= "\\" then
      depth = depth - 1
      if depth == 0 then return s:sub(i + 1, j - 1), j + 1 end
    end
  end
  return nil, i
end

-- one argument: {group}, a single command like \pi, or a single character
local function texArg(s, i)
  while s:sub(i, i):match("%s") do i = i + 1 end
  local c = s:sub(i, i)
  if c == "{" then return braceGroup(s, i) end
  if c == "\\" then
    local cmd = s:match("^\\[%a]+", i) or s:sub(i, i + 1)
    return cmd, i + #cmd
  end
  return c, i + 1
end

local function texToPlain(s)
  local out, i, n = {}, 1, #s
  local function push(x) out[#out + 1] = x end
  while i <= n do
    local c = s:sub(i, i)
    if c == "\\" then
      local cmd = s:match("^\\(%a+)", i)
      if cmd then
        i = i + 1 + #cmd
        if cmd == "frac" or cmd == "dfrac" or cmd == "tfrac" then
          local a, b
          a, i = texArg(s, i)
          b, i = texArg(s, i)
          push("((" .. texToPlain(a or "") .. ")/(" .. texToPlain(b or "") .. "))")
        elseif cmd == "sqrt" then
          local root
          if s:sub(i, i) == "[" then
            local close = s:find("]", i, true)
            if close then root = s:sub(i + 1, close - 1); i = close + 1 end
          end
          local a
          a, i = texArg(s, i)
          if root then
            push("root(" .. texToPlain(a or "") .. "," .. texToPlain(root) .. ")")
          else
            push("sqrt(" .. texToPlain(a or "") .. ")")
          end
        elseif cmd == "text" or cmd == "mathrm" or cmd == "mathit" or cmd == "operatorname" or cmd == "textrm" then
          local a
          a, i = texArg(s, i)
          push(" " .. texToPlain(a or "") .. " ")
        elseif TEX_FUNCTIONS[cmd] then
          -- \sin x  ->  sin(x)
          local j = i
          while s:sub(j, j):match("%s") do j = j + 1 end
          local nextc = s:sub(j, j)
          if nextc == "(" or s:match("^\\left", j) then
            push(cmd)
          else
            local a
            a, i = texArg(s, j)
            -- take a whole number or name, not just its first character
            if a and a:match("^[%w%.]$") then
              local word = s:match("^[%w%.]+", i - 1)
              if word then a = word; i = i - 1 + #word end
            end
            push(cmd .. "(" .. texToPlain(a or "") .. ")")
          end
        elseif TEX_SYMBOLS[cmd] then
          push(TEX_SYMBOLS[cmd])
        else
          push(cmd) -- unknown command: keep its name, the formula check will report it
        end
      else
        local sym = s:sub(i + 1, i + 1)
        if sym == "\\" then push("\n") else push(TEX_SYMBOLS[sym] or "") end
        i = i + 2
      end
    elseif c == "{" then
      local a, ni = braceGroup(s, i)
      if a then push("(" .. texToPlain(a) .. ")"); i = ni else i = i + 1 end
    elseif c == "^" then
      local a
      a, i = texArg(s, i + 1)
      a = texToPlain(a or "")
      push(a:match("^[%w%.]+$") and ("^" .. a) or ("^(" .. a .. ")"))
    elseif c == "_" then
      local a
      a, i = texArg(s, i + 1)
      push("_" .. texToPlain(a or ""):gsub("[^%w]", ""))
    elseif c == "$" or c == "&" then
      i = i + 1
    else
      push(c)
      i = i + 1
    end
  end
  return table.concat(out)
end

--- Converts a LaTeX formula (as typed in the TeX tool) to the plain form parseFormula reads.
function M.fromLatex(tex)
  tex = tex:gsub("\\%[", ""):gsub("\\%]", ""):gsub("\\%(", "("):gsub("\\%)", ")")
  -- v(t) = \begin{cases} 3t & 0 \le t \le 0.6 \\ 1.8 & 0.6 \le t \le 2 \end{cases}
  --   ->  one line per case:  v(t) = 3t, 0 <= t <= 0.6  /  v(t) = 1.8, 0.6 <= t <= 2
  local lhs, body, rest = tex:match("^(.-)=%s*\\begin%s*{cases}(.-)\\end%s*{cases}(.*)$")
  if lhs then
    local rows = {}
    for row in (body .. "\\\\"):gmatch("(.-)\\\\") do
      if not row:match("^%s*$") then
        local expr, cond = row:match("^(.-)&(.*)$")
        rows[#rows + 1] = lhs .. "=" .. (expr or row) .. (cond and (", " .. cond) or "")
      end
    end
    -- anything after the cases (", -2 \le x") applies to every case: give it a line of its own
    rest = rest:gsub("^%s*[,;]?", "")
    tex = table.concat(rows, " \\\\ ") .. (rest:match("%S") and (" \\\\ " .. rest) or "")
  end
  return texToPlain(tex)
end

---------------------------------------------------------------------------
-- Reading TeX elements from Xournal++'s clipboard data ("application/xournal")
---------------------------------------------------------------------------

--- Returns a list of { text, x, y, width, height } for the TeX elements in the data, or nil, error.
function M.readClipboardTex(data)
  if type(data) ~= "string" or #data < 4 then return nil, "no data" end
  local pos, n = 1, #data
  local stack = {} -- open objects: { name, strings = {}, doubles = {}, children = {} }
  local root = { strings = {}, doubles = {}, children = {} }
  local found = {}
  local function cur() return stack[#stack] or root end
  local pendingName = false
  while pos <= n - 1 do
    local tag = data:sub(pos, pos + 1)
    pos = pos + 2
    if tag == "_{" then
      local obj = { strings = {}, doubles = {}, children = {} }
      local parent = cur()
      parent.children[#parent.children + 1] = obj
      stack[#stack + 1] = obj
      pendingName = true
    elseif tag == "_}" then
      local obj = table.remove(stack)
      if not obj then return nil, "bad structure" end
      if obj.name == "TexImage" then
        local el = obj.children[1] or { doubles = {} }
        found[#found + 1] = { text = obj.strings[1] or "", x = el.doubles[1] or 0, y = el.doubles[2] or 0,
          width = obj.doubles[1] or 0, height = obj.doubles[2] or 0 }
      end
    elseif tag == "_s" or tag == "_m" then
      if pos + 7 > n then return nil, "truncated" end
      local len = string.unpack("<i8", data, pos)
      pos = pos + 8
      if len < 0 or len > n - pos + 1 then return nil, "truncated" end
      local str = data:sub(pos, pos + len - 1)
      pos = pos + len
      if pendingName then
        cur().name = str
        pendingName = false
      else
        local o = cur()
        o.strings[#o.strings + 1] = str
      end
    elseif tag == "_d" then
      if pos + 7 > n then return nil, "truncated" end
      local o = cur()
      o.doubles[#o.doubles + 1] = string.unpack("<d", data, pos)
      pos = pos + 8
    elseif tag == "_i" or tag == "_u" then
      pos = pos + 4
    elseif tag == "_l" then
      pos = pos + 8
    elseif tag == "_b" then
      if pos + 15 > n then return nil, "truncated" end
      local len, width = string.unpack("<i8i8", data, pos)
      pos = pos + 16
      -- (a negative or overflowing size would send the reader backwards, possibly forever)
      if len < 0 or width < 0 or (width > 0 and len > (n - pos + 1) // width) then return nil, "truncated" end
      pos = pos + len * width
    else
      return nil, "unknown data"
    end
  end
  return found
end

---------------------------------------------------------------------------
-- Numbers and axes
---------------------------------------------------------------------------

local function finite(v) return type(v) == "number" and v == v and v ~= huge and v ~= -huge end
M.finite = finite

--- A "nice" step (1, 2 or 5 times a power of ten) close to the given raw step.
function M.niceStep(raw)
  if not finite(raw) or raw <= 0 then return 1 end
  local p = 10 ^ floor(math.log(raw, 10))
  local f = raw / p
  local nice
  if f < 1.5 then nice = 1 elseif f < 3.5 then nice = 2 elseif f < 7.5 then nice = 5 else nice = 10 end
  return nice * p
end

--- Formats a tick value without floating-point noise (0.30000000000000004 -> 0.3, -0 -> 0).
function M.formatNumber(v, step)
  if step and abs(v) < abs(step) * 1e-9 then v = 0 end
  local s = string.format("%.6g", v)
  if s:find("e") then
    local mant, ex = s:match("^(.-)e(.*)$")
    s = mant .. "e" .. tostring(tonumber(ex))
  end
  if s == "-0" then s = "0" end
  return s
end

local function clamp(v, lo, hi) return math.max(lo, math.min(hi, v)) end

--- Formats a key value (a range end, a crossing) with up to 4 significant digits: 1.8, 0.6, 51, 0.3333.
function M.formatKey(v)
  local s = string.format("%.4g", v)
  if s:find("e") then
    local mant, ex = s:match("^(.-)e(.*)$")
    s = mant .. "e" .. tostring(tonumber(ex))
  end
  if s == "-0" then s = "0" end
  return s
end

---------------------------------------------------------------------------
-- Sampling and clipping
---------------------------------------------------------------------------

--- Evaluates the curve only at lo, lo + step, lo + 2 step, ... up to hi. Same results as M.sample.
function M.sampleSteps(fx, fy, lo, hi, step)
  local pts, used = {}, {}
  local n = floor((hi - lo) / step + 1e-9)
  for k = 0, n do
    local t = lo + k * step
    if abs(t) < step * 1e-9 then t = 0.0 end -- -5 + 5 * 1 can come out as 1e-16
    local x, y = fx(t), fy(t)
    if x and y then pts[#pts + 1] = { x, y } else pts[#pts + 1] = false end
    used[#used + 1] = t
  end
  return pts, used
end

--- Evaluates the curve. Returns a list where each entry is {x, y} or false (undefined),
--- and the list of parameter values they belong to.
function M.sample(fx, fy, tMin, tMax, samples)
  samples = finite(samples) and clamp(floor(samples), 2, 20000) or 300
  local ts = {}
  for i = 0, samples - 1 do
    ts[#ts + 1] = tMin + (tMax - tMin) * i / (samples - 1)
  end
  -- Also sample every whole number in the range: some formulas, like (-2)^x, only exist there
  if tMax - tMin <= 2000 then
    for k = ceil(tMin), floor(tMax) do ts[#ts + 1] = k + 0.0 end
    table.sort(ts)
  end
  local pts, used, last = {}, {}, nil
  for _, t in ipairs(ts) do
    if t ~= last then
      local x, y = fx(t), fy(t)
      if x and y then pts[#pts + 1] = { x, y } else pts[#pts + 1] = false end
      used[#used + 1] = t
      last = t
    end
  end
  return pts, used
end

-- Liang–Barsky: clips segment p->q to the window, returns the clipped endpoints or nil.
local function clipSegment(x0, y0, x1, y1, w)
  local dx, dy = x1 - x0, y1 - y0
  local u0, u1 = 0, 1
  local checks = {
    { -dx, x0 - w.xMin }, { dx, w.xMax - x0 },
    { -dy, y0 - w.yMin }, { dy, w.yMax - y0 },
  }
  for _, c in ipairs(checks) do
    local p, q = c[1], c[2]
    if p == 0 then
      if q < 0 then return nil end
    else
      local r = q / p
      if p < 0 then
        if r > u1 then return nil end
        if r > u0 then u0 = r end
      else
        if r < u0 then return nil end
        if r < u1 then u1 = r end
      end
    end
  end
  return x0 + u0 * dx, y0 + u0 * dy, x0 + u1 * dx, y0 + u1 * dy, u0 > 0, u1 < 1
end

--- Splits the sampled points into polylines inside the window (in graph coordinates).
--- Undefined points and parts outside the window break the curve.
function M.polylines(pts, w)
  local lines, cur = {}, nil
  local function finish()
    if cur and #cur >= 2 then lines[#lines + 1] = cur end
    cur = nil
  end
  -- A point whose neighbours are undefined (e.g. (-2)^x at whole numbers) becomes a dot
  for i = 1, #pts do
    local p = pts[i]
    if p and not pts[i - 1] and not pts[i + 1] and #pts > 1
        and p[1] >= w.xMin and p[1] <= w.xMax and p[2] >= w.yMin and p[2] <= w.yMax then
      lines[#lines + 1] = { { p[1], p[2] }, { p[1], p[2] } }
    end
  end
  for i = 1, #pts - 1 do
    local p, q = pts[i], pts[i + 1]
    if p and q then
      local ax, ay, bx, by, startCut, endCut = clipSegment(p[1], p[2], q[1], q[2], w)
      if ax then
        if startCut or not cur then
          finish()
          cur = { { ax, ay } }
        end
        cur[#cur + 1] = { bx, by }
        if endCut then finish() end
      else
        finish()
      end
    else
      finish()
    end
  end
  finish()
  return lines
end

--- Clips several polylines (lists of points) to a window.
function M.polylinesOf(lines, w)
  local out = {}
  for _, line in ipairs(lines) do
    for _, l in ipairs(M.polylines(line, w)) do out[#out + 1] = l end
  end
  return out
end

---------------------------------------------------------------------------
-- Building the drawing
---------------------------------------------------------------------------

local ARROW = 8          -- arrow length in pt
local DOT_RADIUS = 1.8   -- pt, for graphs that only exist at whole numbers
local MAX_DOTS = 2000    -- whole numbers checked at most
local MAX_LINES = 60     -- more ticks/grid lines than this per axis gets a coarser step
local MIN_TICKS = 4      -- fewer ticks than this per axis gets a finer step
local MAX_KEY_POINTS = 10 -- key values marked at most
local MAX_STEPS = 2000   -- points of a graph drawn in steps

-- Picks the tick step for one axis. Returns step, wasAdjusted.
-- smallest 1/2/5 x 10^k that is >= v
local function niceStepUp(v)
  local p = 10 ^ floor(math.log(v, 10))
  for _, m in ipairs({ 1, 2, 5, 10 }) do
    if m * p >= v * (1 - 1e-9) then return finite(m * p) and m * p or v end
  end
  return finite(10 * p) and 10 * p or v
end

-- Picks the tick step for one axis so ticks are at least minGap pt apart on the page.
-- Returns step, wasAdjusted.
local function tickStep(unit, lo, hi, lengthPt, minGap)
  local range = hi - lo
  local minStep = range / math.max(lengthPt, 1) * minGap -- divide first: range * minGap may overflow
  if not finite(unit) or unit <= 0 or range / unit > MAX_LINES or unit < minStep then
    return niceStepUp(math.max(minStep, range / MAX_LINES)), true
  end
  -- too few ticks to read the graph (0 <= t <= 2 with a step of 1): use a finer step, if there's room
  if range / unit < MIN_TICKS then
    local finer = math.max(M.niceStep(range / (MIN_TICKS + 2)), niceStepUp(minStep))
    if finer < unit then return finer, true end
  end
  return unit, false
end

local function tickValues(step, lo, hi)
  local vals = {}
  for k = ceil(lo / step - 1e-9), floor(hi / step + 1e-9) do
    vals[#vals + 1] = k * step
  end
  return vals
end

local function textWidth(text, size) return #text * size * 0.6 end

--- Builds everything to draw.
--- cfg fields:
---   exprX, exprY, tMin, tMax, samples
---   autoFit, xMin, xMax, yMin, yMax, xUnit, yUnit
---   width, height (pt), cx, cy (centre of the graph on the page, pt)
---   axes, axisLabels, labelX, labelY, axisColor, axisWidth, axisStyle
---   ticks, numbers, tickHeight
---   grid, gridColor, gridWidth, gridStyle
---   fontName, fontSize
--- Returns result {strokes, texts, window, notes} or nil, error message.
function M.build(cfg)
  local curveDefs = cfg.curves or { { exprX = cfg.exprX, exprY = cfg.exprY } }
  local var = cfg.var
  -- Curves of the same group are pieces of one function (v(t) = 3t for 0 <= t <= 0.6, v(t) = 1.8 after that):
  -- they share a colour and a name. Otherwise every curve is a graph of its own.
  local groupOf, groupNames, groupCount = {}, {}, 0
  do
    local seen = {}
    for i, c in ipairs(curveDefs) do
      local key = c.group or i
      if not seen[key] then
        groupCount = groupCount + 1
        seen[key] = groupCount
        groupNames[groupCount] = c.name
      end
      groupOf[i] = seen[key]
    end
  end
  local single = groupCount == 1
  local funcs = {}
  for i, c in ipairs(curveDefs) do
    local fx, errX = M.compile(c.exprX, var)
    if not fx then return nil, (#curveDefs == 1 and "" or ("Formula " .. i .. ", ")) .. "x part: " .. errX end
    local fy, errY = M.compile(c.exprY, var)
    if not fy then return nil, (#curveDefs == 1 and "" or ("Formula " .. i .. ": ")) .. errY end
    funcs[i] = { fx, fy }
  end

  local tMin, tMax = cfg.tMin, cfg.tMax
  if not (finite(tMin) and finite(tMax)) or tMin >= tMax then
    return nil, "The start of the range must be smaller than the end."
  end
  if not finite(tMax - tMin) then
    return nil, "The range is too large to draw."
  end
  if not (cfg.width > 0 and cfg.height > 0) then
    return nil, "Width and height must be bigger than 0."
  end
  -- each curve may have its own range (pieces of a piecewise function); the rest use the overall one
  local ranges = {}
  for i, c in ipairs(curveDefs) do
    local lo, hi = tMin, tMax
    if c.tMin or c.tMax then lo, hi = M.resolveRange(c, tMin, tMax) end
    if not (finite(lo) and finite(hi) and lo < hi and finite(hi - lo)) then
      return nil, (#curveDefs == 1 and "" or ("Formula " .. i .. ": ")) .. "the start of the range must be smaller than the end."
    end
    ranges[i] = { lo, hi }
  end

  local sampled, sampledTs = {}, {}
  for i, f in ipairs(funcs) do
    local tMin, tMax = ranges[i][1], ranges[i][2]
    local step = curveDefs[i].step
    if step and (tMax - tMin) / step > MAX_STEPS then
      return nil, string.format("Steps of %s from %s to %s are too many to draw (at most %d).",
        M.formatKey(step), M.formatKey(tMin), M.formatKey(tMax), MAX_STEPS)
    end
    local pts, ts
    if step then
      pts, ts = M.sampleSteps(f[1], f[2], tMin, tMax, step)
    else
      pts, ts = M.sample(f[1], f[2], tMin, tMax, cfg.samples or 300)
    end
    -- When the curve only exists on a small part of the range (sqrt(x) for -50 <= x <= 3), most samples are
    -- wasted and the curve looks angular: sample that part again, as finely as the whole range.
    local first, last
    for j = 1, #pts do
      if pts[j] then first = first or j; last = j end
    end
    if not step and first and ts[last] - ts[first] < 0.5 * (tMax - tMin) then
      local a, b = ts[math.max(first - 1, 1)], ts[math.min(last + 1, #ts)]
      if b > a then pts, ts = M.sample(f[1], f[2], a, b, cfg.samples or 300) end
    end
    sampled[i], sampledTs[i] = pts, ts
  end
  local notes = {}

  -- conditions such as y > 0 cut the curve exactly where they stop holding
  local regions = {}
  for i, def in ipairs(curveDefs) do
    local c = def.clip or cfg.clip or {}
    regions[i] = { xMin = c.xMin or -huge, xMax = c.xMax or huge, yMin = c.yMin or -huge, yMax = c.yMax or huge }
  end
  local pieces, anyDefined = {}, false
  for i, pts in ipairs(sampled) do
    for _, p in ipairs(pts) do if p then anyDefined = true break end end
    pieces[i] = M.polylines(pts, regions[i])
  end

  -- Some functions only exist at whole numbers, e.g. (-2)^x. Evaluate those exactly and draw them as dots
  -- wherever the sampled curve doesn't already pass.
  local dots, anyDots = {}, false
  local function inside(x, y, r) return x >= r.xMin and x <= r.xMax and y >= r.yMin and y <= r.yMax end
  for i, f in ipairs(funcs) do
    dots[i] = {}
    local pts = sampled[i]
    local tMin, tMax, region = ranges[i][1], ranges[i][2], regions[i]
    local indexOf = {} -- whole numbers among the sampled parameter values -> their index
    for j, t in ipairs(sampledTs[i]) do
      if t == floor(t) then indexOf[t] = j end
    end
    local isolated = {} -- points the sampling already drew as tiny dots; drawn as proper dots instead
    if curveDefs[i].step then
      -- a graph in steps: a dot at every step, joined by the straight lines already sampled
      for _, p in ipairs(pts) do
        if p and inside(p[1], p[2], region) then
          dots[i][#dots[i] + 1] = { p[1], p[2] }
          isolated[#isolated + 1] = p
          anyDefined = true
        end
      end
    elseif tMax - tMin <= MAX_DOTS then
      for k = ceil(tMin), floor(tMax) do
        local x, y = f[1](k + 0.0), f[2](k + 0.0)
        if x and y and inside(x, y, region) then
          -- is k already on a drawn segment of the sampled curve?
          local j = indexOf[k + 0.0]
          if not (j and pts[j] and (pts[j - 1] or pts[j + 1])) then
            dots[i][#dots[i] + 1] = { x, y }
            if j and pts[j] then isolated[#isolated + 1] = pts[j] end
            anyDefined, anyDots = true, true
          end
        end
      end
    end
    if #isolated > 0 then
      local drop = {}
      for _, p in ipairs(isolated) do drop[p[1] .. "," .. p[2]] = true end
      local kept = {}
      for _, line in ipairs(pieces[i]) do
        local a, b = line[1], line[2]
        if not (#line == 2 and a[1] == b[1] and a[2] == b[2] and drop[a[1] .. "," .. a[2]]) then
          kept[#kept + 1] = line
        end
      end
      pieces[i] = kept
    end
  end
  if not anyDefined then
    return nil, "The function isn't defined anywhere in that range."
  end
  if anyDots then
    notes[#notes + 1] = "Parts of the graph only exist at whole numbers and are drawn as dots."
  end

  -- coordinate window
  local w
  if cfg.autoFit then
    local xMin, xMax, yMin, yMax = huge, -huge, huge, -huge
    local function include(p)
      xMin, xMax = math.min(xMin, p[1]), math.max(xMax, p[1])
      yMin, yMax = math.min(yMin, p[2]), math.max(yMax, p[2])
    end
    for _, lines in ipairs(pieces) do
      for _, line in ipairs(lines) do
        for _, p in ipairs(line) do include(p) end
      end
    end
    for _, list in ipairs(dots) do
      for _, p in ipairs(list) do include(p) end
    end
    if xMin == huge then
      return nil, "No part of the curve meets all the conditions."
    end
    -- The arrowhead covers the end of each axis, so leave a bit more room there: otherwise the number at the
    -- end of the range (7 in 0 <= t <= 7) has no room
    local function endRoom(lengthPt)
      if not cfg.axes then return 0.05 end
      local q = math.min(0.4, (2 * ARROW + 6) / math.max(lengthPt, 1))
      return math.max(0.05, 1.1 * q / (1 - q))
    end
    local function pad(lo, hi, hiRoom)
      local span = hi - lo
      if span < 1e-12 * math.max(1, abs(lo), abs(hi)) then -- constant: give it some room
        local r = math.max(1, abs(lo) * 0.1)
        return lo - r, hi + r
      end
      return lo - 0.05 * span, hi + hiRoom * span
    end
    xMin, xMax = pad(xMin, xMax, endRoom(cfg.width))
    yMin, yMax = pad(yMin, yMax, endRoom(cfg.height))
    w = { xMin = xMin, xMax = xMax, yMin = yMin, yMax = yMax }
    if not (finite(xMin) and finite(xMax) and finite(yMin) and finite(yMax)
        and finite(xMax - xMin) and finite(yMax - yMin)) then
      return nil, "The values are too large to draw. Try a smaller range or a fixed coordinate window."
    end
  else
    w = { xMin = cfg.xMin, xMax = cfg.xMax, yMin = cfg.yMin, yMax = cfg.yMax }
    if not (w.xMin < w.xMax) then return nil, "xmin must be smaller than xmax." end
    if not (w.yMin < w.yMax) then return nil, "ymin must be smaller than ymax." end
    if not (finite(w.xMax - w.xMin) and finite(w.yMax - w.yMin)) then
      return nil, "The coordinate window is too large to draw."
    end
  end

  -- page placement
  local left, top = cfg.cx - cfg.width / 2, cfg.cy - cfg.height / 2
  local right, bottom = left + cfg.width, top + cfg.height
  local sx = cfg.width / (w.xMax - w.xMin)
  local sy = cfg.height / (w.yMax - w.yMin)
  local function X(x) return left + (x - w.xMin) * sx end
  local function Y(y) return top + (w.yMax - y) * sy end

  -- axes sit at 0, or at the window edge when 0 is outside the window
  local axisXval = clamp(0, w.yMin, w.yMax) -- the horizontal axis is at y = this
  local axisYval = clamp(0, w.xMin, w.xMax) -- the vertical axis is at x = this
  local ax, ay = X(axisYval), Y(axisXval)

  local strokes, texts = {}, {}
  local axisColor, gridColor = cfg.axisColor or 0, cfg.gridColor or 0x9a9996

  -- numbers need more room than bare ticks
  local fontSize = cfg.fontSize or 8
  local gapX = cfg.numbers and math.max(12, 3.2 * fontSize) or 6
  local gapY = cfg.numbers and math.max(10, 1.8 * fontSize) or 6
  local xStep, xAdj = tickStep(cfg.xUnit, w.xMin, w.xMax, cfg.width, gapX)
  local yStep, yAdj = tickStep(cfg.yUnit, w.yMin, w.yMax, cfg.height, gapY)
  if (cfg.ticks or cfg.grid or cfg.numbers) and (xAdj or yAdj) then
    notes[#notes + 1] = string.format("Tick spacing adjusted to %s (x) and %s (y).",
      M.formatNumber(xStep), M.formatNumber(yStep))
  end
  local xTicks, yTicks = tickValues(xStep, w.xMin, w.xMax), tickValues(yStep, w.yMin, w.yMax)
  local arrowRoom = cfg.axes and 2 * ARROW or 0

  if cfg.grid then
    for _, v in ipairs(xTicks) do
      local px = X(v)
      strokes[#strokes + 1] = { x = { px, px }, y = { top, bottom }, width = cfg.gridWidth,
        color = gridColor, lineStyle = cfg.gridStyle, tool = "pen" }
    end
    for _, v in ipairs(yTicks) do
      local py = Y(v)
      strokes[#strokes + 1] = { x = { left, right }, y = { py, py }, width = cfg.gridWidth,
        color = gridColor, lineStyle = cfg.gridStyle, tool = "pen" }
    end
  end

  if cfg.axes then
    local axisStroke = function(xs, ys, style)
      return { x = xs, y = ys, width = cfg.axisWidth, color = axisColor, lineStyle = style or "plain", tool = "pen" }
    end
    strokes[#strokes + 1] = axisStroke({ left, right }, { ay, ay }, cfg.axisStyle)
    strokes[#strokes + 1] = axisStroke({ ax, ax }, { bottom, top }, cfg.axisStyle)
    strokes[#strokes + 1] = axisStroke({ right - ARROW, right, right - ARROW },
      { ay - 0.6 * ARROW, ay, ay + 0.6 * ARROW })
    strokes[#strokes + 1] = axisStroke({ ax - 0.6 * ARROW, ax, ax + 0.6 * ARROW },
      { top + ARROW, top, top + ARROW })
  end

  local th = cfg.tickHeight or 4
  if cfg.ticks then
    for _, v in ipairs(xTicks) do
      local px = X(v)
      if px < right - arrowRoom and abs(v - axisYval) > xStep * 1e-9 then
        strokes[#strokes + 1] = { x = { px, px }, y = { ay - th / 2, ay + th / 2 }, width = cfg.axisWidth,
          color = axisColor, lineStyle = "plain", tool = "pen" }
      end
    end
    for _, v in ipairs(yTicks) do
      local py = Y(v)
      if py > top + arrowRoom and abs(v - axisXval) > yStep * 1e-9 then
        strokes[#strokes + 1] = { x = { ax - th / 2, ax + th / 2 }, y = { py, py }, width = cfg.axisWidth,
          color = axisColor, lineStyle = "plain", tool = "pen" }
      end
    end
  end

  -- Key values: where a range written in the formula starts and ends (0.6 and 1.8 in "v(t) = 3t, 0 <= t <= 0.6"),
  -- and where two graphs cross. They get their numbers on the axes and dashed guide lines.
  local keyPts = {}
  if cfg.axes and cfg.numbers and cfg.keyValues ~= false then
    local spanX, spanY = w.xMax - w.xMin, w.yMax - w.yMin
    local function addKey(x, y)
      if not (finite(x) and finite(y)) or #keyPts >= MAX_KEY_POINTS then return end
      if x < w.xMin or x > w.xMax or y < w.yMin or y > w.yMax then return end
      for _, k in ipairs(keyPts) do
        if abs(k[1] - x) <= 1e-9 * spanX and abs(k[2] - y) <= 1e-9 * spanY then return end
      end
      keyPts[#keyPts + 1] = { x, y }
    end
    for i, c in ipairs(curveDefs) do
      local lo, hi = c.tMin, c.tMax
      if not (lo or hi) then lo, hi = cfg.givenTMin, cfg.givenTMax end
      for _, t in ipairs({ lo or false, hi or false }) do
        if t then addKey(funcs[i][1](t), funcs[i][2](t)) end
      end
    end
    -- maximums and minimums inside each graph's range (the top of 20t - t^2 at t = 10)
    -- (not for graphs in steps: their highest point is one of the steps, between them is only a straight line)
    for i, c in ipairs(curveDefs) do
      if c.exprX == var and not c.step then
        local f = funcs[i][2]
        local lo, hi = ranges[i][1], ranges[i][2]
        local N = 400
        local ts, ys = {}, {}
        for k = 0, N do
          ts[k] = lo + (hi - lo) * k / N
          ys[k] = f(ts[k])
        end
        local yMinS, yMaxS = huge, -huge
        for k = 0, N do
          if ys[k] then yMinS, yMaxS = math.min(yMinS, ys[k]), math.max(yMaxS, ys[k]) end
        end
        local tol = 1e-9 * math.max(1, abs(yMinS), abs(yMaxS))
        local extremes = {}
        for k = 1, N - 1 do
          local a, b, d = ys[k - 1], ys[k], ys[k + 1]
          if a and b and d then
            local isMax = b > a + tol and b >= d + tol
            local isMin = b < a - tol and b <= d - tol
            if isMax or isMin then
              -- golden-section search between the neighbouring samples
              local x0, x1 = ts[k - 1], ts[k + 1]
              local g = (math.sqrt(5) - 1) / 2
              local function better(u, v) -- is f(u) more extreme than f(v)?
                local fu, fv = f(u), f(v)
                if not (fu and fv) then return false end
                if isMax then return fu > fv else return fu < fv end
              end
              local m1, m2 = x1 - g * (x1 - x0), x0 + g * (x1 - x0)
              for _ = 1, 80 do
                if better(m1, m2) then x1 = m2 else x0 = m1 end
                m1, m2 = x1 - g * (x1 - x0), x0 + g * (x1 - x0)
              end
              local tm = (x0 + x1) / 2
              if f(tm) then extremes[#extremes + 1] = tm end
            end
          end
        end
        -- a wiggly graph (sin(10x)) would get a key value at every bump: then mark none
        if #extremes <= 4 then
          for _, t in ipairs(extremes) do addKey(funcs[i][1](t), f(t)) end
        end
      end
    end
    -- where each graph crosses the axes: its zeros (v = 0 where the motion turns around) and its value at 0
    for i, c in ipairs(curveDefs) do
      if c.exprX == var and not c.step then
        local f = funcs[i][2]
        local lo, hi = ranges[i][1], ranges[i][2]
        local zeros = {}
        local N = 400
        local prevT, prevV
        for k = 0, N do
          local t = lo + (hi - lo) * k / N
          local v = f(t)
          if v == 0 then
            zeros[#zeros + 1] = t
            v = nil -- don't count it again with the next sample
          elseif v and prevV and (prevV < 0) ~= (v < 0) then
            local t0, t1, v0 = prevT, t, prevV
            for _ = 1, 60 do
              local m = (t0 + t1) / 2
              local vm = f(m)
              if not vm then break end
              if vm == 0 then t0, t1 = m, m break end
              if (vm < 0) == (v0 < 0) then t0, v0 = m, vm else t1 = m end
            end
            local tz = (t0 + t1) / 2
            local vz = f(tz)
            -- a real zero, not a jump across a pole (1/x)
            if vz and abs(vz) <= 1e-6 * (1 + abs(prevV) + abs(v)) then zeros[#zeros + 1] = tz end
          end
          prevT, prevV = t, v
        end
        if #zeros <= 4 then
          for _, t in ipairs(zeros) do addKey(t, 0) end
        end
        if lo < 0 and hi > 0 then addKey(0, f(0)) end
      end
    end
    -- crossings of graphs of the same variable (not of pieces of one function)
    for i = 1, #curveDefs do
      for j = i + 1, #curveDefs do
        local fi, fj = funcs[i][2], funcs[j][2]
        if groupOf[i] ~= groupOf[j] and curveDefs[i].exprX == var and curveDefs[j].exprX == var
            and not curveDefs[i].step and not curveDefs[j].step then
          local lo, hi = math.max(ranges[i][1], ranges[j][1]), math.min(ranges[i][2], ranges[j][2])
          local found = {}
          local function diff(t)
            local a, b = fi(t), fj(t)
            return a and b and a - b
          end
          if lo < hi then
            local N = 400
            local prevT, prevD
            for k = 0, N do
              local t = lo + (hi - lo) * k / N
              local d = diff(t)
              if d == 0 then
                found[#found + 1] = t
                d = nil -- don't count it again with the next sample
              elseif d and prevD and (prevD < 0) ~= (d < 0) then
                local t0, t1, d0 = prevT, t, prevD
                for _ = 1, 60 do
                  local m = (t0 + t1) / 2
                  local dm = diff(m)
                  if not dm then break end
                  if dm == 0 then t0, t1 = m, m break end
                  if (dm < 0) == (d0 < 0) then t0, d0 = m, dm else t1 = m end
                end
                local tc = (t0 + t1) / 2
                local a, dc = fi(tc), diff(tc)
                -- a real crossing, not a jump across a pole (1/x)
                if a and dc and abs(dc) <= 1e-6 * (1 + abs(a)) then found[#found + 1] = tc end
              end
              prevT, prevD = t, d
            end
          end
          if #found <= 4 then
            for _, t in ipairs(found) do addKey(funcs[i][1](t), fi(t)) end
          end
        end
      end
    end
    -- dashed guide lines from each key point to the axes, and tick marks
    for _, k in ipairs(keyPts) do
      local px, py = X(k[1]), Y(k[2])
      local onYAxis, onXAxis = abs(px - ax) < 0.5, abs(py - ay) < 0.5
      if not onYAxis and not onXAxis then
        strokes[#strokes + 1] = { x = { px, px }, y = { py, ay }, width = 0.5, color = gridColor, lineStyle = "dash",
          tool = "pen" }
        strokes[#strokes + 1] = { x = { px, ax }, y = { py, py }, width = 0.5, color = gridColor, lineStyle = "dash",
          tool = "pen" }
      end
      if not onYAxis then
        strokes[#strokes + 1] = { x = { px, px }, y = { ay - th / 2, ay + th / 2 }, width = cfg.axisWidth,
          color = axisColor, lineStyle = "plain", tool = "pen" }
      end
      if not onXAxis then
        strokes[#strokes + 1] = { x = { ax - th / 2, ax + th / 2 }, y = { py, py }, width = cfg.axisWidth,
          color = axisColor, lineStyle = "plain", tool = "pen" }
      end
    end
  end

  -- the curves, drawn last so they sit on top. One curve: current pen colour and width.
  -- Several curves: each gets its own colour so they can be told apart.
  local PALETTE = { 0x1a5fb4, 0xc01c28, 0x26a269, 0xe66100, 0x613583, 0x865e3c }
  local curves = 0
  local curveEnds = {} -- where each group's curve ends on the page (its piece reaching furthest), for its name
  local endT = {}
  for i, lines in ipairs(pieces) do
    for _, line in ipairs(M.polylinesOf(lines, w)) do
      local xs, ys = {}, {}
      for j, p in ipairs(line) do xs[j], ys[j] = X(p[1]), Y(p[2]) end
      local g = groupOf[i]
      if not endT[g] or ranges[i][2] >= endT[g] then
        curveEnds[g], endT[g] = { xs[#xs], ys[#ys] }, ranges[i][2]
      end
      -- fill = -1: never fill the area under the curve, even if the pen has "fill" turned on
      local s = { x = xs, y = ys, tool = "pen", fill = -1 }
      if cfg.curveColor then s.color = cfg.curveColor end
      if not single then s.color = PALETTE[(groupOf[i] - 1) % #PALETTE + 1] end
      if cfg.curveWidth then s.width = cfg.curveWidth end
      strokes[#strokes + 1] = s
      curves = curves + 1
    end
    -- isolated points: small filled circles in the curve's colour
    for _, p in ipairs(dots[i]) do
      if inside(p[1], p[2], w) then
        local cx, cy = X(p[1]), Y(p[2])
        local xs, ys = {}, {}
        for j = 0, 12 do
          local a = 2 * math.pi * j / 12
          xs[#xs + 1], ys[#ys + 1] = cx + DOT_RADIUS * math.cos(a), cy + DOT_RADIUS * math.sin(a)
        end
        local s = { x = xs, y = ys, tool = "pen", fill = 255, width = 0.5 }
        if cfg.curveColor then s.color = cfg.curveColor end
        if not single then s.color = PALETTE[(groupOf[i] - 1) % #PALETTE + 1] end
        strokes[#strokes + 1] = s
        curves = curves + 1
      end
    end
  end
  if curves == 0 then
    return nil, "No part of the graph falls inside the coordinate window. Try auto-fit."
  end

  local size = cfg.fontSize or 8
  local font = { name = cfg.fontName or "Sans", size = size }
  if cfg.numbers then
    -- key values first: the regular numbers make way for them
    local keyX, keyY = {}, {} -- {page position, text}
    local function addLabel(list, pos, s)
      for _, l in ipairs(list) do if abs(l[1] - pos) < 0.5 then return end end
      list[#list + 1] = { pos, s }
    end
    for _, k in ipairs(keyPts) do
      local px, py = X(k[1]), Y(k[2])
      if abs(px - ax) >= 0.5 then addLabel(keyX, px, M.formatKey(k[1])) end
      if abs(py - ay) >= 0.5 then addLabel(keyY, py, M.formatKey(k[2])) end
    end
    local function freeX(px, s)
      for _, l in ipairs(keyX) do
        if abs(l[1] - px) < (textWidth(s, size) + textWidth(l[2], size)) / 2 + 2 then return false end
      end
      return true
    end
    local function freeY(py)
      for _, l in ipairs(keyY) do if abs(l[1] - py) < 1.1 * size then return false end end
      return true
    end
    for _, v in ipairs(xTicks) do
      local px = X(v)
      local s = M.formatNumber(v, xStep)
      if px < right - arrowRoom and abs(v - axisYval) > xStep * 1e-9 and freeX(px, s) then
        texts[#texts + 1] = { text = s, font = font, color = axisColor,
          x = px - textWidth(s, size) / 2, y = ay + th / 2 + 1 }
      end
    end
    for _, v in ipairs(yTicks) do
      local py = Y(v)
      if py > top + arrowRoom and abs(v - axisXval) > yStep * 1e-9 and freeY(py) then
        local s = M.formatNumber(v, yStep)
        texts[#texts + 1] = { text = s, font = font, color = axisColor,
          x = ax - th / 2 - 2 - textWidth(s, size), y = py - 0.75 * size }
      end
    end
    -- key labels that would overlap each other: keep the first
    local placedX, placedY = {}, {}
    for _, l in ipairs(keyX) do
      local ok = true
      for _, p in ipairs(placedX) do
        if abs(p[1] - l[1]) < (textWidth(p[2], size) + textWidth(l[2], size)) / 2 + 2 then ok = false end
      end
      if ok then
        placedX[#placedX + 1] = l
        texts[#texts + 1] = { text = l[2], font = font, color = axisColor,
          x = l[1] - textWidth(l[2], size) / 2, y = ay + th / 2 + 1 }
      end
    end
    for _, l in ipairs(keyY) do
      local ok = true
      for _, p in ipairs(placedY) do if abs(p[1] - l[1]) < 1.1 * size then ok = false end end
      if ok then
        placedY[#placedY + 1] = l
        texts[#texts + 1] = { text = l[2], font = font, color = axisColor,
          x = ax - th / 2 - 2 - textWidth(l[2], size), y = l[1] - 0.75 * size }
      end
    end
  end
  -- several named graphs (f(t) = ..., g(t) = ...): write each name at the end of its curve, in its colour
  if not single and cfg.axes and cfg.axisLabels then
    local seen, named = {}, true
    for g = 1, groupCount do
      local nm = groupNames[g]
      if not nm or seen[nm] then named = false break end
      seen[nm] = true
    end
    if named then
      local placed = {}
      local lineH = 1.3 * size
      local function overlaps(x, y, tw)
        for _, b in ipairs(placed) do
          if x < b[1] + b[3] and b[1] < x + tw and y < b[2] + lineH and b[2] < y + lineH then return true end
        end
        return false
      end
      for g = 1, groupCount do
        local e, name = curveEnds[g], groupNames[g]
        if e then
          local tw = textWidth(name, size)
          local tx = e[1] + 3
          if tx + tw > right then tx = e[1] - tw - 3 end
          local ty = clamp(e[2] - lineH, top, bottom - lineH)
          -- curves that end at the same place (or close): put the names below each other
          local tries = 0
          while overlaps(tx, ty, tw) and tries < groupCount do
            ty = ty + lineH
            if ty > bottom - lineH then ty = top end
            tries = tries + 1
          end
          placed[#placed + 1] = { tx, ty, tw }
          texts[#texts + 1] = { text = name, font = font, color = PALETTE[(g - 1) % #PALETTE + 1], x = tx, y = ty }
        end
      end
    end
  end

  local labelX = (cfg.labelX and cfg.labelX ~= "") and cfg.labelX or cfg.autoLabelX
  local labelY = (cfg.labelY and cfg.labelY ~= "") and cfg.labelY or cfg.autoLabelY
  if cfg.axes and cfg.axisLabels then
    if labelX and labelX ~= "" then
      texts[#texts + 1] = { text = labelX, font = font, color = axisColor,
        x = right - textWidth(labelX, size), y = ay - 0.6 * ARROW - 1.6 * size }
    end
    if labelY and labelY ~= "" then
      texts[#texts + 1] = { text = labelY, font = font, color = axisColor,
        x = ax + 0.6 * ARROW + 2, y = top - 0.4 * size }
    end
  end

  -- final safety net: never hand non-finite coordinates to Xournal++
  for _, s in ipairs(strokes) do
    for i = 1, #s.x do
      if not (finite(s.x[i]) and finite(s.y[i])) then
        return nil, "Internal error: a coordinate came out invalid. Please report this."
      end
    end
  end

  return { strokes = strokes, texts = texts, window = w, notes = notes,
    xStep = xStep, yStep = yStep }
end

return M
