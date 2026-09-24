-- tilewm example config: copy to ~/.config/tilewm/init.lua and tweak.
-- Reload a running compositor with Alt+Shift+R (or SIGHUP).
-- Key names follow xkb keysyms ("Return", "space", "q", "1" ...).
-- Modifiers: Alt, Ctrl, Shift, Super (Logo/Win/Mod4 also work).

config = {
    gaps = 8,      -- pixels around each window and the screen edge
    mfact = 0.60,  -- master column width fraction (0.05 .. 0.95)
    nmaster = 1,   -- windows in the master column
    workspaces = 4 -- number of workspaces (1 .. 9)
}

bind("Alt", "Return", "spawn-terminal")
bind("Alt", "j", "focus-next")
bind("Alt", "k", "focus-prev")
bind("Alt", "space", "toggle-floating")
bind("Alt", "q", "close")
bind("Alt+Shift", "e", "quit")
bind("Alt+Shift", "r", "reload-config")
bind("Ctrl+Alt", "q", "quit")

for i = 1, 4 do
    bind("Alt", tostring(i), "workspace", i)
    bind("Alt+Shift", tostring(i), "move-to-workspace", i)
end
