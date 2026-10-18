#include "hotkey.h"
#include <cstdio>

static int failures=0;
static void Check(bool ok,const char* name) { printf("%s %s\n",ok?"PASS":"FAIL",name);if(!ok)++failures; }

int main() {
    HotkeyGate gate;
    gate.Reset(false);
    Check(gate.Poll(true,true,true,110),"polling triggers first press");
    Check(!gate.Poll(true,true,true,120),"held key does not repeat");
    Check(!gate.Message(true,true,105),"poll-first native message is deduplicated");
    gate.Poll(false,true,true,130);
    Check(!gate.Message(false,true,105),"late native message after release is deduplicated");
    Check(gate.Message(true,true,150),"new native press is accepted");
    Check(!gate.Poll(true,true,true,155),"message-first polling is deduplicated");
    Check(gate.Message(false,true,165),"quick native tap works without a sampled release");
    Check(!gate.Message(false,true,105),"older polled message stays consumed after newer native messages");
    Check(!gate.Poll(false,true,true,170),"release does not trigger");
    gate.Poll(false,true,true,2200);
    Check(gate.Message(false,true,0x80000100u),"idle expiry permits native events after long uptime");
    gate.Reset(true);
    Check(!gate.Poll(true,true,true,200) && !gate.Message(true,true,201),"configuration press waits for release");
    gate.Poll(false,true,true,210);
    Check(gate.Poll(true,true,true,220),"fresh press works after configuration release");
    gate.Reset(false);
    Check(!gate.Poll(true,false,true,300),"wrong modifiers do not trigger");
    Check(!gate.Poll(true,true,true,310),"releasing extra modifiers while holding a key does not trigger");
    gate.Poll(false,true,true,320);
    Check(!gate.Poll(true,true,false,330) && !gate.Message(true,false,330),"modal UI disables hotkeys");
    Check(!gate.Poll(true,true,true,340),"closing modal UI does not activate held key");
    gate.Reset(false);
    Check(gate.Poll(true,true,true,0xfffffff0u),"polling before tick wrap works");
    Check(!gate.Message(true,true,0xffffffefu),"same press before tick wrap is deduplicated");
    gate.Poll(false,true,true,2);
    Check(gate.Message(true,true,4),"new press after tick wrap works");
    return failures?1:0;
}
