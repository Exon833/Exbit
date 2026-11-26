// The exports of Wine's dxgi.dll (made by tools/winefix_exports.sh from Wine's own DLL: run it again
// for a newer Wine): EXPORT(index, ordinal, name).
EXPORT(0, 1, CreateDXGIFactory)
EXPORT(1, 2, CreateDXGIFactory1)
EXPORT(2, 3, CreateDXGIFactory2)
EXPORT(3, 4, DXGID3D10CreateDevice)
EXPORT(4, 5, DXGID3D10RegisterLayers)
EXPORT(5, 6, DXGIGetDebugInterface1)
