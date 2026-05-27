#pragma once

struct DlssSettings
{
    bool Enabled = false;
    int  Mode = 3; // 0=Off, 1=Max Performance, 2=Balanced, 3=Max Quality, 4=Ultra Performance, 5=Ultra Quality, 6=DLAA
    bool ResetHistory = false;
};