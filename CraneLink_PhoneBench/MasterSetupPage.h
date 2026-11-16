#pragma once

constexpr char masterSetupHead[] PROGMEM = R"CLSETUP(<!doctype html>
<html lang="en">
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>CraneLink handheld setup</title>
<style>
:root{color-scheme:dark;background:#0d171b;color:#edf6f3;font:1rem/1.55 system-ui,sans-serif}
*{box-sizing:border-box} main{max-width:36rem;margin:auto;padding:1.25rem}
h1{line-height:1.12;font-size:clamp(1.8rem,7vw,2.6rem)} h2{font-size:1.2rem}
form,.notice{border:1px solid #3c5358;border-radius:.75rem;padding:1rem;margin:1rem 0;background:#17262b}
label{display:block;margin:.7rem 0;font-weight:600}
input,button{font:inherit;border:1px solid #3c5358;border-radius:.4rem;padding:.6rem .8rem}
input{display:block;width:100%;margin:.25rem 0;background:#0d171b;color:inherit}
button{background:#a5e8ce;color:#10251c;font-weight:700;cursor:pointer}
code{overflow-wrap:anywhere;color:#a5e8ce} :focus-visible{outline:2px solid #a5c7ff;outline-offset:3px}
</style>
<main>
<h1>Handheld setup</h1>
<p>Handheld control is paused in setup mode.</p>
)CLSETUP";

constexpr char masterSetupFields[] PROGMEM = R"CLSETUP(
<h2>Add or update a receiver</h2>
<label>Label <input name="name" placeholder="MH14AB4292" maxlength="32" pattern="[A-Za-z0-9\-]{1,32}" required></label>
<label>Receiver station MAC <input name="mac" placeholder="AA:BB:CC:DD:EE:FF" maxlength="17" pattern="([A-Fa-f0-9]{2}:){5}[A-Fa-f0-9]{2}" autocapitalize="characters" spellcheck="false" required></label>
<label>Shared key <input name="key" type="password" maxlength="32" pattern="[A-Fa-f0-9]{32}" autocomplete="off" required></label>
<button type="submit">Save receiver</button></form>
)CLSETUP";
