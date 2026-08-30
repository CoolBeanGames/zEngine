#include "EditorShell.h"

#include "Renderer.h"
#include "FbxImporter.h"

#include <windowsx.h>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string_view>
#include <chrono>

namespace
{
    std::wstring WideText(const std::string& text)
    {
        const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring result(count, L' ');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count);
        return result;
    }
    constexpr wchar_t EditorWindowClass[] = L"zEngineEditorWindow";
    constexpr wchar_t ViewportWindowClass[] = L"zEngineViewportWindow";

    constexpr int OptionsBarHeight = 32;
    constexpr int StatusBarHeight = 27;
    constexpr int PanelHeaderHeight = 30;
    constexpr int SplitterSize = 5;
    constexpr int MinimumSideWidth = 150;
    constexpr int MinimumViewportWidth = 260;
    constexpr int MinimumMediaHeight = 120;
    constexpr int MinimumUpperHeight = 180;

    constexpr COLORREF EditorBackground = RGB(32, 34, 38);
    constexpr COLORREF PanelBackground = RGB(40, 42, 47);
    constexpr COLORREF HeaderBackground = RGB(47, 49, 55);
    constexpr COLORREF BorderColor = RGB(20, 21, 24);
    constexpr COLORREF TextColor = RGB(214, 216, 221);
    constexpr COLORREF MutedTextColor = RGB(145, 149, 158);
    constexpr COLORREF FieldColor = RGB(52, 54, 60);
    constexpr COLORREF SelectionColor = RGB(54, 83, 119);

    void FillRectangle(HDC deviceContext, const RECT& rectangle, const COLORREF color)
    {
        const HBRUSH brush = CreateSolidBrush(color);
        FillRect(deviceContext, &rectangle, brush);
        DeleteObject(brush);
    }

    void DrawBorder(HDC deviceContext, const RECT& rectangle, const COLORREF color)
    {
        const HPEN pen = CreatePen(PS_SOLID, 1, color);
        const HGDIOBJ previousPen = SelectObject(deviceContext, pen);
        const HGDIOBJ previousBrush = SelectObject(deviceContext, GetStockObject(HOLLOW_BRUSH));
        Rectangle(deviceContext, rectangle.left, rectangle.top, rectangle.right, rectangle.bottom);
        SelectObject(deviceContext, previousBrush);
        SelectObject(deviceContext, previousPen);
        DeleteObject(pen);
    }

    void DrawTextLabel(
        HDC deviceContext,
        const std::wstring_view text,
        RECT rectangle,
        const COLORREF color,
        const UINT format = DT_LEFT | DT_SINGLELINE | DT_VCENTER)
    {
        SetBkMode(deviceContext, TRANSPARENT);
        SetTextColor(deviceContext, color);
        DrawTextW(deviceContext, text.data(), static_cast<int>(text.size()), &rectangle, format);
    }

    void DrawPanel(HDC deviceContext, const RECT& rectangle, const std::wstring_view title)
    {
        FillRectangle(deviceContext, rectangle, PanelBackground);
        RECT header = rectangle;
        header.bottom = std::min(header.bottom, header.top + PanelHeaderHeight);
        FillRectangle(deviceContext, header, HeaderBackground);
        DrawBorder(deviceContext, rectangle, BorderColor);
        header.left += 11;
        DrawTextLabel(deviceContext, title, header, TextColor);
    }

    void DrawField(HDC deviceContext, const int x, const int y, const int width, const std::wstring_view value)
    {
        RECT field{x, y, x + width, y + 23};
        FillRectangle(deviceContext, field, FieldColor);
        DrawBorder(deviceContext, field, BorderColor);
        field.left += 7;
        DrawTextLabel(deviceContext, value, field, TextColor);
    }
}

EditorShell::EditorShell(const HINSTANCE instance)
    : instance_(instance)
{
    WNDCLASSEXW editorClass{};
    editorClass.cbSize = sizeof(editorClass);
    editorClass.style = CS_HREDRAW | CS_VREDRAW;
    editorClass.lpfnWndProc = WindowProcedure;
    editorClass.hInstance = instance_;
    editorClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    editorClass.hbrBackground = nullptr;
    editorClass.lpszClassName = EditorWindowClass;
    if (!RegisterClassExW(&editorClass))
    {
        throw std::runtime_error("Could not register the editor window class.");
    }

    WNDCLASSEXW viewportClass{};
    viewportClass.cbSize = sizeof(viewportClass);
    viewportClass.style = CS_OWNDC;
    viewportClass.lpfnWndProc = ViewportProcedure;
    viewportClass.hInstance = instance_;
    viewportClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    viewportClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    viewportClass.lpszClassName = ViewportWindowClass;
    if (!RegisterClassExW(&viewportClass))
    {
        UnregisterClassW(EditorWindowClass, instance_);
        throw std::runtime_error("Could not register the viewport window class.");
    }

    uiFont_ = CreateFontW(
        -14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    headerFont_ = CreateFontW(
        -14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

EditorShell::~EditorShell()
{
    renderer_.reset();
    if (window_ && IsWindow(window_))
    {
        DestroyWindow(window_);
    }
    if (uiFont_)
    {
        DeleteObject(uiFont_);
    }
    if (headerFont_)
    {
        DeleteObject(headerFont_);
    }
    UnregisterClassW(ViewportWindowClass, instance_);
    UnregisterClassW(EditorWindowClass, instance_);
}

HWND EditorShell::Create(const int showCommand, const std::filesystem::path& projectDirectory)
{
    RECT windowArea{0, 0, 1440, 900};
    AdjustWindowRectEx(&windowArea, WS_OVERLAPPEDWINDOW, FALSE, 0);
    window_ = CreateWindowExW(
        0, EditorWindowClass, L"zEngine Editor", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT,
        windowArea.right - windowArea.left, windowArea.bottom - windowArea.top,
        nullptr, nullptr, instance_, this);
    if (!window_)
    {
        throw std::runtime_error("Could not create the zEngine editor window.");
    }

    CreateViewport();
    std::array<wchar_t, 32768> executable{};
    const DWORD pathLength = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!pathLength || pathLength == executable.size()) throw std::runtime_error("Cannot locate the project directory.");
    assetsDirectory_ = (projectDirectory.empty()
        ? std::filesystem::path(executable.data()).parent_path() / "Project"
        : std::filesystem::absolute(projectDirectory)) / "Assets";
    RefreshAssets();
    DragAcceptFiles(window_, TRUE);
    RECT client{};
    GetClientRect(window_, &client);
    Layout(static_cast<std::uint32_t>(client.right), static_cast<std::uint32_t>(client.bottom));
    ShowWindow(window_, showCommand);
    UpdateWindow(window_);
    return window_;
}

void EditorShell::CreateViewport()
{
    viewportWindow_ = CreateWindowExW(
        0, ViewportWindowClass, L"Scene Viewport",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        0, 0, 1, 1, window_, nullptr, instance_, nullptr);
    if (!viewportWindow_)
    {
        throw std::runtime_error("Could not create the Direct3D viewport window.");
    }
}

void EditorShell::InitializeRenderer()
{
    if (!viewportWindow_ || requestedViewportWidth_ == 0 || requestedViewportHeight_ == 0)
    {
        throw std::runtime_error("The editor viewport has no drawable area.");
    }
    renderer_ = std::make_unique<Renderer>();
    renderer_->Initialize(viewportWindow_, requestedViewportWidth_, requestedViewportHeight_);
    rendererWidth_ = requestedViewportWidth_;
    rendererHeight_ = requestedViewportHeight_;
}

void EditorShell::Render()
{
    PollAssetWork();
    if (!renderer_ || requestedViewportWidth_ == 0 || requestedViewportHeight_ == 0)
    {
        return;
    }

    if (requestedViewportWidth_ != rendererWidth_ || requestedViewportHeight_ != rendererHeight_)
    {
        if (requestedViewportWidth_ > 0 && requestedViewportHeight_ > 0)
        {
            renderer_->Resize(requestedViewportWidth_, requestedViewportHeight_);
            rendererWidth_ = requestedViewportWidth_;
            rendererHeight_ = requestedViewportHeight_;
        }
    }
    renderer_->Render();
}

void EditorShell::Layout(const std::uint32_t width, const std::uint32_t height)
{
    const int clientWidth = static_cast<int>(width);
    const int clientHeight = static_cast<int>(height);
    if (clientWidth <= 0 || clientHeight <= 0)
    {
        return;
    }

    optionsBar_ = RECT{0, 0, clientWidth, std::min(OptionsBarHeight, clientHeight)};
    statusBar_ = RECT{0, std::max(0, clientHeight - StatusBarHeight), clientWidth, clientHeight};

    const int workspaceTop = optionsBar_.bottom;
    const int workspaceBottom = statusBar_.top;
    const int workspaceHeight = std::max(0, workspaceBottom - workspaceTop);
    mediaLibraryHeight_ = std::clamp(
        mediaLibraryHeight_, MinimumMediaHeight,
        std::max(MinimumMediaHeight, workspaceHeight - MinimumUpperHeight - SplitterSize));

    mediaLibrary_ = RECT{
        0, std::max(workspaceTop, workspaceBottom - mediaLibraryHeight_), clientWidth, workspaceBottom};
    mediaSplitter_ = RECT{0, std::max<LONG>(workspaceTop, mediaLibrary_.top - SplitterSize),
                          clientWidth, mediaLibrary_.top};
    const int upperBottom = mediaSplitter_.top;

    const int maximumSideSpace = std::max(0, clientWidth - MinimumViewportWidth - (SplitterSize * 2));
    sceneBrowserWidth_ = std::clamp(sceneBrowserWidth_, MinimumSideWidth,
                                   std::max(MinimumSideWidth, maximumSideSpace - MinimumSideWidth));
    inspectorWidth_ = std::clamp(inspectorWidth_, MinimumSideWidth,
                                 std::max(MinimumSideWidth, maximumSideSpace - sceneBrowserWidth_));
    if (sceneBrowserWidth_ + inspectorWidth_ > maximumSideSpace)
    {
        inspectorWidth_ = std::max(MinimumSideWidth, maximumSideSpace - sceneBrowserWidth_);
    }

    sceneBrowser_ = RECT{0, workspaceTop, std::min(sceneBrowserWidth_, clientWidth), upperBottom};
    sceneSplitter_ = RECT{sceneBrowser_.right, workspaceTop,
                          std::min<LONG>(sceneBrowser_.right + SplitterSize, clientWidth), upperBottom};
    inspector_ = RECT{std::max<LONG>(sceneSplitter_.right, clientWidth - inspectorWidth_), workspaceTop,
                      clientWidth, upperBottom};
    inspectorSplitter_ = RECT{std::max(sceneSplitter_.right, inspector_.left - SplitterSize), workspaceTop,
                              inspector_.left, upperBottom};
    viewportPanel_ = RECT{sceneSplitter_.right, workspaceTop, inspectorSplitter_.left, upperBottom};
    viewportContent_ = viewportPanel_;
    viewportContent_.top = std::min(viewportContent_.bottom, viewportContent_.top + PanelHeaderHeight);

    const int viewportWidth = std::max<LONG>(0, viewportContent_.right - viewportContent_.left);
    const int viewportHeight = std::max<LONG>(0, viewportContent_.bottom - viewportContent_.top);
    requestedViewportWidth_ = static_cast<std::uint32_t>(viewportWidth);
    requestedViewportHeight_ = static_cast<std::uint32_t>(viewportHeight);
    if (viewportWindow_)
    {
        SetWindowPos(viewportWindow_, nullptr,
                     viewportContent_.left, viewportContent_.top, viewportWidth, viewportHeight,
                     SWP_NOACTIVATE | SWP_NOZORDER);
    }

    InvalidateRect(window_, nullptr, FALSE);
}

void EditorShell::Paint()
{
    PAINTSTRUCT paint{};
    const HDC windowContext = BeginPaint(window_, &paint);
    RECT client{};
    GetClientRect(window_, &client);
    const int width = std::max<LONG>(1, client.right - client.left);
    const int height = std::max<LONG>(1, client.bottom - client.top);

    const HDC bufferContext = CreateCompatibleDC(windowContext);
    const HBITMAP bufferBitmap = CreateCompatibleBitmap(windowContext, width, height);
    const HGDIOBJ previousBitmap = SelectObject(bufferContext, bufferBitmap);
    SelectObject(bufferContext, uiFont_);
    FillRectangle(bufferContext, client, EditorBackground);

    // Options bar
    FillRectangle(bufferContext, optionsBar_, RGB(35, 37, 41));
    RECT brand = optionsBar_;
    brand.left += 10;
    brand.right = brand.left + 26;
    SelectObject(bufferContext, headerFont_);
    DrawTextLabel(bufferContext, L"z", brand, RGB(108, 166, 232), DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    SelectObject(bufferContext, uiFont_);
    constexpr std::array<std::wstring_view, 5> menus{L"File", L"Edit", L"View", L"Window", L"Help"};
    int menuX = 44;
    for (const std::wstring_view menu : menus)
    {
        RECT menuRectangle{menuX, optionsBar_.top, menuX + 62, optionsBar_.bottom};
        DrawTextLabel(bufferContext, menu, menuRectangle, TextColor, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        menuX += (menu == L"Window") ? 70 : 55;
    }
    RECT projectName{optionsBar_.right - 240, optionsBar_.top, optionsBar_.right - 12, optionsBar_.bottom};
    DrawTextLabel(bufferContext, L"Untitled Project", projectName, MutedTextColor, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    RECT optionLine{0, optionsBar_.bottom - 1, optionsBar_.right, optionsBar_.bottom};
    FillRectangle(bufferContext, optionLine, BorderColor);

    // Main panels
    SelectObject(bufferContext, headerFont_);
    DrawPanel(bufferContext, sceneBrowser_, L"Scene Browser");
    DrawPanel(bufferContext, viewportPanel_, L"Scene");
    DrawPanel(bufferContext, inspector_, L"Inspector");
    DrawPanel(bufferContext, mediaLibrary_, L"Media Library");
    SelectObject(bufferContext, uiFont_);

    // Scene browser mock content
    const int sceneX = sceneBrowser_.left + 13;
    int sceneY = sceneBrowser_.top + PanelHeaderHeight + 12;
    RECT sceneRoot{sceneX, sceneY, sceneBrowser_.right - 8, sceneY + 25};
    DrawTextLabel(bufferContext, L"\u25be  Untitled Scene", sceneRoot, TextColor);
    sceneY += 30;
    RECT selected{sceneBrowser_.left + 5, sceneY, sceneBrowser_.right - 5, sceneY + 27};
    FillRectangle(bufferContext, selected, SelectionColor);
    selected.left = sceneX + 18;
    DrawTextLabel(bufferContext, L"\u25a0  " + sceneName_, selected, TextColor, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    sceneY += 36;
    RECT hint{sceneX, sceneY, sceneBrowser_.right - 10, sceneY + 40};
    DrawTextLabel(bufferContext, L"Scene objects will appear here", hint, MutedTextColor, DT_LEFT | DT_WORDBREAK);

    // Viewport header controls. Rendering occurs in the child window below this strip.
    const int centerX = (viewportPanel_.left + viewportPanel_.right) / 2;
    RECT playButton{centerX - 42, viewportPanel_.top + 4, centerX - 14, viewportPanel_.top + 26};
    RECT pauseButton{centerX - 12, viewportPanel_.top + 4, centerX + 16, viewportPanel_.top + 26};
    RECT stepButton{centerX + 18, viewportPanel_.top + 4, centerX + 46, viewportPanel_.top + 26};
    for (const RECT button : {playButton, pauseButton, stepButton})
    {
        FillRectangle(bufferContext, button, FieldColor);
        DrawBorder(bufferContext, button, BorderColor);
    }
    DrawTextLabel(bufferContext, L"\u25b6", playButton, TextColor, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    DrawTextLabel(bufferContext, L"\u2016", pauseButton, TextColor, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    DrawTextLabel(bufferContext, L"\u25b8|", stepButton, TextColor, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    // Inspector mock content
    const int inspectorX = inspector_.left + 12;
    const int inspectorContentWidth = std::max<LONG>(40, inspector_.right - inspectorX - 12);
    int inspectorY = inspector_.top + PanelHeaderHeight + 12;
    RECT objectName{inspectorX, inspectorY, inspector_.right - 12, inspectorY + 25};
    DrawTextLabel(bufferContext, sceneName_, objectName, TextColor, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    inspectorY += 34;
    RECT sectionHeader{inspectorX, inspectorY, inspector_.right - 12, inspectorY + 24};
    FillRectangle(bufferContext, sectionHeader, HeaderBackground);
    sectionHeader.left += 7;
    DrawTextLabel(bufferContext, L"\u25be  Transform", sectionHeader, TextColor);
    inspectorY += 32;
    constexpr std::array<std::wstring_view, 3> propertyNames{L"Position", L"Rotation", L"Scale"};
    constexpr std::array<std::wstring_view, 3> propertyValues{L"X  0    Y  0    Z  0", L"X  0    Y  0    Z  0", L"X  1    Y  1    Z  1"};
    for (std::size_t index = 0; index < propertyNames.size(); ++index)
    {
        RECT propertyLabel{inspectorX, inspectorY, inspectorX + 68, inspectorY + 23};
        DrawTextLabel(bufferContext, propertyNames[index], propertyLabel, MutedTextColor);
        DrawField(bufferContext, inspectorX + 72, inspectorY,
                  std::max(30, inspectorContentWidth - 72), propertyValues[index]);
        inspectorY += 29;
    }
    inspectorY += 8;
    RECT rendererSection{inspectorX, inspectorY, inspector_.right - 12, inspectorY + 24};
    FillRectangle(bufferContext, rendererSection, HeaderBackground);
    rendererSection.left += 7;
    DrawTextLabel(bufferContext, L"\u25b8  Mesh Renderer", rendererSection, TextColor);

    // Media library mock content
    const int mediaTop = mediaLibrary_.top + PanelHeaderHeight;
    const int folderPaneWidth = std::clamp<LONG>((mediaLibrary_.right - mediaLibrary_.left) / 5, 150, 250);
    RECT folderPane{mediaLibrary_.left + 1, mediaTop, mediaLibrary_.left + folderPaneWidth, mediaLibrary_.bottom - 1};
    FillRectangle(bufferContext, folderPane, RGB(36, 38, 43));
    RECT assetsLabel{folderPane.left + 12, folderPane.top + 12, folderPane.right - 8, folderPane.top + 36};
    DrawTextLabel(bufferContext, L"\u25be  Assets", assetsLabel, TextColor);
    RECT folderHint{folderPane.left + 30, folderPane.top + 43, folderPane.right - 8, folderPane.top + 68};
    DrawTextLabel(bufferContext, L"Project Files", folderHint, MutedTextColor);
    RECT addFolder{mediaLibrary_.right - 116, mediaLibrary_.top + 4, mediaLibrary_.right - 10, mediaLibrary_.top + 26};
    FillRectangle(bufferContext, addFolder, FieldColor);
    DrawBorder(bufferContext, addFolder, BorderColor);
    DrawTextLabel(bufferContext, L"+ Add Folder", addFolder, TextColor, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    RECT dropArea{folderPane.right + 20, mediaTop + 20, mediaLibrary_.right - 20, mediaLibrary_.bottom - 20};
    DrawBorder(bufferContext, dropArea, RGB(72, 75, 83));
    RECT libraryHint{dropArea.left + 8, dropArea.top, dropArea.right - 8, dropArea.top + 26};
    DrawTextLabel(bufferContext, L"Drop FBX files here  |  Drag an asset into Scene to preview  |  Scroll to browse", libraryHint,
                  MutedTextColor, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    const RECT list = AssetListRectangle();
    const int saved = SaveDC(bufferContext);
    IntersectClipRect(bufferContext, list.left, list.top, list.right, list.bottom);
    if (assets_.empty())
        DrawTextLabel(bufferContext, L"No imported models", list, MutedTextColor, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    for (int index = firstAsset_; index < static_cast<int>(assets_.size()); ++index)
    {
        RECT row{list.left, list.top + (index - firstAsset_) * 28, list.right,
                 list.top + (index - firstAsset_ + 1) * 28};
        if (row.top >= list.bottom) break;
        if (index == selectedAsset_) FillRectangle(bufferContext, row, SelectionColor);
        row.left += 8;
        DrawTextLabel(bufferContext, L"FBX   " + assets_[index].parent_path().filename().wstring(), row, TextColor,
                      DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }
    RestoreDC(bufferContext, saved);

    // Status bar and dormant progress indicator
    FillRectangle(bufferContext, statusBar_, RGB(30, 32, 36));
    RECT statusText{statusBar_.left + 10, statusBar_.top, statusBar_.right - 370, statusBar_.bottom};
    DrawTextLabel(bufferContext, status_, statusText, TextColor, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    RECT taskText{statusBar_.right - 360, statusBar_.top, statusBar_.right - 190, statusBar_.bottom};
    DrawTextLabel(bufferContext, assetWork_.valid() ? L"Processing asset..." : L"No tasks running", taskText,
                  MutedTextColor, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    RECT progressTrack{statusBar_.right - 176, statusBar_.top + 8, statusBar_.right - 12, statusBar_.bottom - 8};
    FillRectangle(bufferContext, progressTrack, FieldColor);
    DrawBorder(bufferContext, progressTrack, BorderColor);
    if (assetWork_.valid())
    {
        const LONG offset = static_cast<LONG>((GetTickCount64() / 15) % 120);
        RECT activity{progressTrack.left + 2 + offset, progressTrack.top + 2,
                      progressTrack.left + 30 + offset, progressTrack.bottom - 2};
        FillRectangle(bufferContext, activity, RGB(63, 126, 201));
    }

    BitBlt(windowContext, 0, 0, width, height, bufferContext, 0, 0, SRCCOPY);
    SelectObject(bufferContext, previousBitmap);
    DeleteObject(bufferBitmap);
    DeleteDC(bufferContext);
    EndPaint(window_, &paint);
}

EditorShell::DragTarget EditorShell::HitTestSplitter(const POINT position) const
{
    if (PtInRect(&sceneSplitter_, position))
    {
        return DragTarget::SceneBrowser;
    }
    if (PtInRect(&inspectorSplitter_, position))
    {
        return DragTarget::Inspector;
    }
    if (PtInRect(&mediaSplitter_, position))
    {
        return DragTarget::MediaLibrary;
    }
    return DragTarget::None;
}

void EditorShell::BeginDrag(const POINT position)
{
    dragTarget_ = HitTestSplitter(position);
    if (dragTarget_ != DragTarget::None)
    {
        SetCapture(window_);
    }
}

RECT EditorShell::AssetListRectangle() const
{
    const int folderWidth = std::clamp(static_cast<int>(mediaLibrary_.right - mediaLibrary_.left) / 5, 150, 250);
    return {mediaLibrary_.left + folderWidth + 21, mediaLibrary_.top + PanelHeaderHeight + 48,
            mediaLibrary_.right - 21, mediaLibrary_.bottom - 21};
}

void EditorShell::RefreshAssets()
{
    assets_.clear();
    if (std::filesystem::exists(assetsDirectory_))
    {
        for (const auto& entry : std::filesystem::directory_iterator(assetsDirectory_))
        {
            if (entry.is_directory() && std::filesystem::is_regular_file(entry.path() / "asset.ready") &&
                std::filesystem::is_regular_file(entry.path() / "model.fbx"))
                assets_.push_back(entry.path() / "model.fbx");
        }
    }
    std::sort(assets_.begin(), assets_.end());
    firstAsset_ = std::clamp(firstAsset_, 0, std::max(0, static_cast<int>(assets_.size()) - 1));
    selectedAsset_ = -1;
}

void EditorShell::ReceiveFiles(HDROP drop)
{
    struct DropOwner { HDROP handle; ~DropOwner() { DragFinish(handle); } } owner{drop};
    POINT location{};
    if (!DragQueryPoint(drop, &location) || !PtInRect(&mediaLibrary_, location))
    {
        status_ = L"Drop FBX files into the Media Library first.";
        InvalidateRect(window_, &statusBar_, FALSE);
        return;
    }
    const UINT count = DragQueryFileW(drop, 0xffffffff, nullptr, 0);
    for (UINT index = 0; index < count; ++index)
    {
        const UINT length = DragQueryFileW(drop, index, nullptr, 0);
        std::vector<wchar_t> name(static_cast<std::size_t>(length) + 1);
        if (DragQueryFileW(drop, index, name.data(), static_cast<UINT>(name.size())))
            assetJobs_.push_back({std::filesystem::path(name.data()), false});
    }
}

void EditorShell::PollAssetWork()
{
    if (assetWork_.valid() && assetWork_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        try
        {
            auto result = assetWork_.get();
            if (result.replacePreview)
            {
                const auto textureWarnings = renderer_->SetModel(result.model);
                result.warnings = std::move(result.model.warnings);
                result.warnings.insert(result.warnings.end(), textureWarnings.begin(), textureWarnings.end());
                sceneName_ = result.path.parent_path().filename().wstring();
                SetWindowTextW(viewportWindow_, (L"Scene Viewport - " + sceneName_).c_str());
                status_ = L"Previewing " + sceneName_;
            }
            else
            {
                // Cancel a row drag before rebuilding the library order.
                draggedAsset_ = -1;
                if (GetCapture() == window_ && dragTarget_ == DragTarget::None) ReleaseCapture();
                RefreshAssets();
                status_ = L"Imported " + result.path.parent_path().filename().wstring();
            }
            if (!result.warnings.empty())
                status_ += L" (albedo fallback: " + WideText(result.warnings.front()) + L")";
        }
        catch (const std::exception& error)
        {
            status_ = L"Asset operation failed: " + WideText(error.what());
        }
        InvalidateRect(window_, nullptr, FALSE);
    }
    if (!assetWork_.valid() && !assetJobs_.empty())
    {
        const auto job = assetJobs_.front();
        assetJobs_.pop_front();
        status_ = (job.replacePreview ? L"Loading " : L"Importing ") + job.path.filename().wstring();
        const auto directory = assetsDirectory_;
        assetWork_ = std::async(std::launch::async, [job, directory]() {
            AssetResult result;
            result.replacePreview = job.replacePreview;
            result.path = job.path;
            if (job.replacePreview) result.model = FbxImporter::Load(job.path, true);
            else result.path = FbxImporter::Import(job.path, directory, result.warnings);
            return result;
        });
    }
    if (assetWork_.valid() && GetTickCount64() - lastBusyPaint_ > 100)
    {
        lastBusyPaint_ = GetTickCount64();
        InvalidateRect(window_, &statusBar_, FALSE);
    }
}

void EditorShell::BeginAssetDrag(POINT point)
{
    const RECT list = AssetListRectangle();
    if (!PtInRect(&list, point)) return;
    const int index = firstAsset_ + static_cast<int>(point.y - list.top) / 28;
    if (index >= static_cast<int>(assets_.size())) return;
    selectedAsset_ = draggedAsset_ = index;
    assetDragStart_ = point;
    assetDragMoved_ = false;
    SetCapture(window_);
    InvalidateRect(window_, &mediaLibrary_, FALSE);
}

void EditorShell::FinishAssetDrag(POINT point)
{
    if (draggedAsset_ < 0) return;
    if (assetDragMoved_ && PtInRect(&viewportContent_, point))
        assetJobs_.push_back({assets_.at(draggedAsset_), true});
    draggedAsset_ = -1;
    assetDragMoved_ = false;
    ReleaseCapture();
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
}

void EditorShell::UpdateDrag(const POINT position)
{
    if (dragTarget_ == DragTarget::None)
    {
        return;
    }

    RECT client{};
    GetClientRect(window_, &client);
    switch (dragTarget_)
    {
    case DragTarget::SceneBrowser:
        sceneBrowserWidth_ = position.x;
        break;
    case DragTarget::Inspector:
        inspectorWidth_ = client.right - position.x;
        break;
    case DragTarget::MediaLibrary:
        mediaLibraryHeight_ = statusBar_.top - position.y;
        break;
    default:
        break;
    }
    Layout(static_cast<std::uint32_t>(std::max(0L, client.right)),
           static_cast<std::uint32_t>(std::max(0L, client.bottom)));
}

void EditorShell::EndDrag()
{
    if (dragTarget_ != DragTarget::None)
    {
        dragTarget_ = DragTarget::None;
        ReleaseCapture();
    }
}

LRESULT CALLBACK EditorShell::WindowProcedure(
    const HWND window, const UINT message, const WPARAM wParam, const LPARAM lParam)
{
    EditorShell* editor = reinterpret_cast<EditorShell*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        const auto* creation = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        editor = static_cast<EditorShell*>(creation->lpCreateParams);
        editor->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(editor));
    }
    try
    {
        return editor ? editor->HandleMessage(window, message, wParam, lParam)
                      : DefWindowProcW(window, message, wParam, lParam);
    }
    catch (const std::exception& error)
    {
        // Never let C++ exceptions cross a Windows callback boundary.
        if (editor)
        {
            editor->status_ = L"Editor error: " + WideText(error.what());
            InvalidateRect(window, &editor->statusBar_, FALSE);
        }
        return 0;
    }
}

LRESULT CALLBACK EditorShell::ViewportProcedure(
    const HWND window, const UINT message, const WPARAM wParam, const LPARAM lParam)
{
    if (message == WM_ERASEBKGND)
    {
        return 1;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT EditorShell::HandleMessage(
    const HWND window, const UINT message, const WPARAM wParam, const LPARAM lParam)
{
    switch (message)
    {
    case WM_DROPFILES:
        ReceiveFiles(reinterpret_cast<HDROP>(wParam));
        return 0;
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
        {
            requestedViewportWidth_ = 0;
            requestedViewportHeight_ = 0;
        }
        else
        {
            Layout(LOWORD(lParam), HIWORD(lParam));
        }
        return 0;
    case WM_GETMINMAXINFO:
    {
        auto* sizing = reinterpret_cast<MINMAXINFO*>(lParam);
        sizing->ptMinTrackSize.x = 800;
        sizing->ptMinTrackSize.y = 600;
        return 0;
    }
    case WM_PAINT:
        Paint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN:
        BeginDrag(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        if (dragTarget_ == DragTarget::None) BeginAssetDrag(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        return 0;
    case WM_MOUSEMOVE:
        if (draggedAsset_ >= 0)
        {
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            assetDragMoved_ = assetDragMoved_ || std::abs(point.x - assetDragStart_.x) >= GetSystemMetrics(SM_CXDRAG) ||
                              std::abs(point.y - assetDragStart_.y) >= GetSystemMetrics(SM_CYDRAG);
            if (assetDragMoved_) SetCursor(LoadCursorW(nullptr, PtInRect(&viewportContent_, point) ? IDC_HAND : IDC_NO));
            return 0;
        }
        if (dragTarget_ != DragTarget::None)
        {
            UpdateDrag(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        FinishAssetDrag(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        EndDrag();
        return 0;
    case WM_MOUSEWHEEL:
    {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(window_, &point);
        if (PtInRect(&mediaLibrary_, point) && draggedAsset_ < 0)
        {
            const RECT list = AssetListRectangle();
            const int visibleRows = std::max(1, static_cast<int>(list.bottom - list.top) / 28);
            firstAsset_ = std::clamp(firstAsset_ - GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA * 3,
                                    0, std::max(0, static_cast<int>(assets_.size()) - visibleRows));
            InvalidateRect(window_, &mediaLibrary_, FALSE);
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        dragTarget_ = DragTarget::None;
        draggedAsset_ = -1;
        assetDragMoved_ = false;
        return 0;
    case WM_SETCURSOR:
    {
        POINT cursor{};
        GetCursorPos(&cursor);
        ScreenToClient(window_, &cursor);
        const DragTarget target = HitTestSplitter(cursor);
        if (target == DragTarget::MediaLibrary)
        {
            SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
            return TRUE;
        }
        if (target == DragTarget::SceneBrowser || target == DragTarget::Inspector)
        {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
            return TRUE;
        }
        break;
    }
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
        {
            if (draggedAsset_ >= 0)
            {
                draggedAsset_ = -1;
                ReleaseCapture();
                return 0;
            }
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_DESTROY:
        viewportWindow_ = nullptr;
        PostQuitMessage(0);
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        window_ = nullptr;
        return DefWindowProcW(window, message, wParam, lParam);
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
