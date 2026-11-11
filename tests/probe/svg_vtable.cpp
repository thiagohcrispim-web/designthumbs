// Sonda temporária: mostra a posição real, no Windows SDK, de cada método sobrecarregado de ID2D1SvgElement.
#include <windows.h>
#include <d2d1_3.h>
#include <d2d1svg.h>
__declspec(noinline) HRESULT P_SetString(ID2D1SvgElement* e) { return e->SetAttributeValue(L"a", D2D1_SVG_ATTRIBUTE_STRING_TYPE_SVG, L"b"); }
__declspec(noinline) HRESULT P_SetPod(ID2D1SvgElement* e, const void* v) { return e->SetAttributeValue(L"a", D2D1_SVG_ATTRIBUTE_POD_TYPE_FLOAT, v, 4); }
__declspec(noinline) HRESULT P_SetAttr(ID2D1SvgElement* e, ID2D1SvgAttribute* a) { return e->SetAttributeValue(L"a", a); }
__declspec(noinline) HRESULT P_GetString(ID2D1SvgElement* e, PWSTR v) { return e->GetAttributeValue(L"a", D2D1_SVG_ATTRIBUTE_STRING_TYPE_SVG, v, 4); }
__declspec(noinline) HRESULT P_GetPod(ID2D1SvgElement* e, void* v) { return e->GetAttributeValue(L"a", D2D1_SVG_ATTRIBUTE_POD_TYPE_FLOAT, v, 4); }
__declspec(noinline) HRESULT P_GetRiid(ID2D1SvgElement* e, void** v) { return e->GetAttributeValue(L"a", IID_IUnknown, v); }
__declspec(noinline) HRESULT P_GetLength(ID2D1SvgElement* e, UINT32* v) { return e->GetAttributeValueLength(L"a", D2D1_SVG_ATTRIBUTE_STRING_TYPE_SVG, v); }
__declspec(noinline) BOOL P_IsSpecified(ID2D1SvgElement* e) { return e->IsAttributeSpecified(L"a"); }
__declspec(noinline) UINT32 P_TextLength(ID2D1SvgElement* e) { return e->GetTextValueLength(); }
int main() { return 0; }
