#pragma once

class UITimeDilator
{
public:

    enum UIMode : u8
    {
        None = 0,
        Inventory = 1 << 0,
        Pda = 1 << 1
    };

    void SetUiTimeFactor(float timeFactor);
    float GetUiTimeFactor() const;

    void SetModeEnability(UIMode mode, bool status);
    bool GetModeEnability(UIMode mode) const;

    void SetCurrentMode(UIMode mode);

private:
    float uiTimeFactor = 1.0;
    Flags8 enabledModes;
    UIMode currMode;

    void startTimeDilation();
    void stopTimeDilation();
};

extern UITimeDilator* TimeDilator();
extern void CloseTimeDilator();
