# Helpers for driving the windowed game: capture only the game window's rectangle, and send it keyboard/mouse input.
param([string]$Action, [string]$Arg1 = "", [string]$Arg2 = "", [int]$ProcId = 0)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class GWin {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT p);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int cmd);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, int dx, int dy, uint data, UIntPtr extra);
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint mapType);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    public static void Key(byte vk, bool down) {
        // SDL reads scancodes, so send them
        byte scan = (byte)MapVirtualKey(vk, 0);
        keybd_event(vk, scan, down ? 0u : 2u, UIntPtr.Zero);
    }
    public static void MouseMove(int dx, int dy) { mouse_event(0x0001, dx, dy, 0, UIntPtr.Zero); }
    public static void MouseLeft(bool down) { mouse_event(down ? 0x0002u : 0x0004u, 0, 0, 0, UIntPtr.Zero); }
}
"@
[void][GWin]::SetProcessDPIAware()

function Get-GameWindow {
    $procs = Get-Process gamecore_template -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 }
    if ($ProcId -ne 0) { $procs = $procs | Where-Object { $_.Id -eq $ProcId } }
    $p = $procs | Select-Object -First 1
    if (-not $p) { throw "no game window found" }
    return $p
}

function Focus-Game($p) {
    [void][GWin]::ShowWindow($p.MainWindowHandle, 9)
    [void][GWin]::SetForegroundWindow($p.MainWindowHandle)
    Start-Sleep -Milliseconds 300
    return ([GWin]::GetForegroundWindow() -eq $p.MainWindowHandle)
}

switch ($Action) {
    "capture" {
        $p = Get-GameWindow
        $focused = Focus-Game $p
        $r = New-Object GWin+RECT
        [void][GWin]::GetClientRect($p.MainWindowHandle, [ref]$r)
        $pt = New-Object GWin+POINT
        [void][GWin]::ClientToScreen($p.MainWindowHandle, [ref]$pt)
        $w = $r.Right - $r.Left; $h = $r.Bottom - $r.Top
        $bmp = New-Object System.Drawing.Bitmap $w, $h
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen($pt.X, $pt.Y, 0, 0, (New-Object System.Drawing.Size $w, $h))
        $bmp.Save($Arg1, [System.Drawing.Imaging.ImageFormat]::Png)
        $g.Dispose(); $bmp.Dispose()
        "captured ${w}x${h} (window focused: $focused) -> $Arg1"
    }
    "key" {
        # Arg1 = virtual key code (decimal), Arg2 = hold time in ms
        $p = Get-GameWindow
        [void](Focus-Game $p)
        $hold = if ($Arg2) { [int]$Arg2 } else { 60 }
        [GWin]::Key([byte][int]$Arg1, $true); Start-Sleep -Milliseconds $hold; [GWin]::Key([byte][int]$Arg1, $false)
        "key $Arg1 held $hold ms"
    }
    "look" {
        # Arg1 = dx, Arg2 = dy (relative mouse motion, sent in small steps)
        $p = Get-GameWindow
        [void](Focus-Game $p)
        $dx = [int]$Arg1; $dy = [int]$Arg2
        for ($i = 0; $i -lt 20; $i++) { [GWin]::MouseMove([int]($dx / 20), [int]($dy / 20)); Start-Sleep -Milliseconds 10 }
        "looked $dx,$dy"
    }
    "click" {
        # Arg1 = x, Arg2 = y in window client coordinates
        $p = Get-GameWindow
        [void](Focus-Game $p)
        $pt = New-Object GWin+POINT
        $pt.X = [int]$Arg1; $pt.Y = [int]$Arg2
        [void][GWin]::ClientToScreen($p.MainWindowHandle, [ref]$pt)
        [void][GWin]::SetCursorPos($pt.X, $pt.Y); Start-Sleep -Milliseconds 150
        [GWin]::MouseMove(1, 0); Start-Sleep -Milliseconds 150
        [GWin]::MouseLeft($true); Start-Sleep -Milliseconds 80; [GWin]::MouseLeft($false)
        "clicked $Arg1,$Arg2"
    }
    "fire" {
        # Arg1 = hold time in ms
        $p = Get-GameWindow
        [void](Focus-Game $p)
        $hold = if ($Arg1) { [int]$Arg1 } else { 1000 }
        [GWin]::MouseLeft($true); Start-Sleep -Milliseconds $hold; [GWin]::MouseLeft($false)
        "fired for $hold ms"
    }
}
