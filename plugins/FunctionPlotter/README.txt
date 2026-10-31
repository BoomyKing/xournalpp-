FunctionPlotter for Xournal++ 1.3
=================================

Plots formulas as normal Xournal++ strokes: with axes, ticks, numbers and
axis names, or just the line on its own.

BUNDLED: in BoomyKing's Xournal++ build (github.com/BoomyKing/xournalpp-) this plugin is
installed and enabled automatically; the steps below are only needed for other builds.
If you installed it yourself before, delete that copy (%LOCALAPPDATA%\xournalpp\plugins\FunctionPlotter),
otherwise both are loaded and every menu entry appears twice.

INSTALL (Windows)
1. Close Xournal++.
2. Press Win+R, type  %LOCALAPPDATA%\xournalpp  and press Enter.
3. Create a folder named  plugins  there if it doesn't exist.
4. Copy the whole FunctionPlotter folder into it, so you end up with
   %LOCALAPPDATA%\xournalpp\plugins\FunctionPlotter\main.lua
5. Start Xournal++. If the Plugin menu doesn't show the FunctionPlotter entries,
   open Plugin > Plugin Manager, tick FunctionPlotter and restart.

ADD THE TOOLBAR BUTTONS
1. Edit > Toolbars > Customize.
2. If it asks whether to create a copy of the predefined toolbar, click Yes.
3. Click the "Plugins" tab on the left of the panel that opens.
4. Drag "Plot formula from text box" (graph icon) and/or
   "Plot formula from text box (line only)" (curve icon) onto a toolbar, then Close.
If the Plugins tab is empty, the plugin isn't loaded: check the Plugin menu, the
folder location above, and Plugin > Plugin Manager.

WRITE A FORMULA, PRESS THE BUTTON
1. Write the formula in a text box, e.g.
      x = 100 - t^2, 0 <= t <= 64
2. Press the toolbar button (or Plugin > Plot formula from text box).
The graph appears under the text box, already selected so you can drag or resize it.
It uses the newest formula text box on the page; if you have several, select the
one you want first (a selected TeX formula works too). Selecting several boxes draws them all on the same axes.

Things you can write:
   y = x^2 - 3x                       (range -3..3 unless you give one)
   x = 100 - t^2, 0 <= t <= 64        (axes are named t and x automatically)
   y = 100 - x^2, 0 <= x <= 64, y > 0 (extra conditions cut the curve where they stop holding)
   y = sqrt(x), x >= 0                (one-sided: the other end comes from the dialog's range)
   f(x) = sin(x) for 0 <= x <= 2pi
   y = x^2; y = 2x + 1                (several graphs, each in its own colour)
   f(t) = 20t - t^2, g(t) = 21 + 10t, 0 <= t <= 7
                                      (several named graphs: each curve gets its name)
   x = cos(t), y = sin(t), 0 <= t <= 2pi, y >= 0    (parametric curve, here the top half)
Only formulas named x and y make a parametric curve; any other names are separate graphs.
   y = -2^x, -10 <= x <= 10, |y| < 100   (absolute values, in formulas and conditions)
   y = (-2)^x, -10 <= x <= 10            (only exists at whole numbers: drawn as dots)
Note: -2^x means -(2^x), like in maths. Write (-2)^x for a negative base.
Conditions: a <= v <= b, v > a, v < b, b >= v >= a, v in [a, b], |v| < c, |v - a| <= c.
Separate them with commas, semicolons, new lines or the word "for".
(|v| > c isn't supported, since it would split the graph in two.)

TeX FORMULAS (the TeX tool)
Select the TeX formula with the selection tool (or Edit > Select All), then press the button.
Write it the normal LaTeX way, e.g.
   y = \frac{1}{2}(100 - x^{2}),\; 0 \le x \le 64,\; y > 0
\frac, \sqrt, \sqrt[3]{}, \cdot, \le, \ge, \pi, \sin x, \left( \right) and \text{for} are understood.
(Reading the TeX formula goes through the clipboard, so after this the clipboard holds
the copied formula.)
Symbols like −  ≤  ²  π  √ also work. 2t, 3(t+1) and 2πx (or 2pix) work without the *.
|x| is the absolute value, same as abs(x); in TeX \left| \right|, \lvert \rvert work too.
Functions: sin cos tan asin acos atan sinh cosh tanh sqrt root(v,n) abs exp
ln log (base 10) log2 floor ceil round sign min max.  Constants: pi, e.

THE DIALOG (Plugin > Plot function...)
Same formulas, plus all the options: range, size (cm by default), "Only the line",
fixed coordinate window, grid, colours, font. Tick "Parametric curve" for separate
x(t) and y(t) boxes. The toolbar buttons use the size and style you last chose here.
A single curve uses your pen settings: colour, thickness and line style
(solid, dashed, dotted...). Several curves get separate colours. Axes use the
style set on the Appearance tab.

CHANGES FROM THE ORIGINAL (github.com/xournalpp/xournalpp/pull/3811)
- Works with Xournal++ 1.3 / Lua 5.5 and the bundled LuaGObject.
- Plot from a text box with toolbar buttons; "line only" option.
- One Formula box instead of x(t)/y(t); ranges and axis names read from the formula;
  several functions at once.
- No more errors where the function is undefined (sqrt of a negative, division by
  zero): the curve just has a gap there.
- Auto-fit no longer divides by zero on constant functions (which produced invalid
  coordinates and could crash on undo).
- Axes stay inside the graph when 0 is outside the window.
- Huge ranges pick a sensible tick spacing instead of drawing thousands of lines.
- Replaced the luaxp parser with a small, sandboxed one with clear error messages.
- Graph placed on the current page and selected; settings remembered; cm/mm units;
  fixed GTK warnings and a checkbox that greyed out the wrong field.

FIXES IN 2.7
- f(t) = ..., g(t) = ... draws two graphs over t (it drew one parametric curve, f against g).
  Each curve is labelled with its name.

FIXES IN 2.6
- 2πx / 2pix is read as 2·π·x instead of failing with "The formula uses pix".
- No stray dot at the end of curves such as sqrt(x); (-2)^x draws each dot once.
- Curves that only exist on part of the range (sqrt(x), -50 <= x <= 3) are drawn smoothly.
- Parametric dialog: a box without "x =" but with a range (cos(t), 0 <= t <= 2pi) is read correctly.
- Saved settings keep backslashes (TeX) and line breaks exactly.
- Very large values give a clear message instead of "Internal error".
- Unknown functions such as sec(x) are named in the error message.
- Damaged clipboard data can no longer make the TeX reading hang.

Original plugin by Roland Lötscher, GPL-2.0-or-later. This modified version is
under the same licence.
