#pragma once

#include <UIAutomation.h>
#include <wrl/client.h>

inline bool valuePropertyUnavailable(IUIAutomation *automation, IUIAutomationElement *element,
                                     HRESULT *propertyStatus)
{
    // CurrentValue can supply UIA's default empty string after the editor is gone.
    VARIANT property;
    VariantInit(&property);
    *propertyStatus = element->GetCurrentPropertyValueEx(UIA_ValueValuePropertyId, TRUE, &property);
    Microsoft::WRL::ComPtr<IUnknown> notSupported;
    const bool unavailable = *propertyStatus == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE)
        || (SUCCEEDED(*propertyStatus) && property.vt == VT_UNKNOWN
            && SUCCEEDED(automation->get_ReservedNotSupportedValue(&notSupported))
            && notSupported && property.punkVal == notSupported.Get());
    VariantClear(&property);
    return unavailable;
}
