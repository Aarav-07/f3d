#include "window_impl.h"

#include "camera_impl.h"
#include "engine.h"
#include "log.h"
#include "options.h"

#include "vtkF3DConfigure.h"

#include "vtkF3DGenericImporter.h"
#include "vtkF3DNoRenderWindow.h"
#include "vtkF3DRenderer.h"

#include <vtkCamera.h>
#include <vtkImageData.h>
#include <vtkImageExport.h>
#include <vtkPNGReader.h>
#include <vtkPointGaussianMapper.h>
#include <vtkRenderWindow.h>
#include <vtkRendererCollection.h>
#include <vtkRenderingOpenGLConfigure.h>
#include <vtkVersion.h>
#include <vtkWindowToImageFilter.h>
#include <vtksys/SystemTools.hxx>

#ifdef VTK_USE_X
#include <vtkXOpenGLRenderWindow.h>
#endif

#ifdef _WIN32
#include <vtkWin32OpenGLRenderWindow.h>
#endif

#ifdef VTK_OPENGL_HAS_EGL
#include <vtkEGLRenderWindow.h>
#endif

#if VTK_VERSION_NUMBER >= VTK_VERSION_CHECK(9, 3, 20240914)
#include <vtkOSOpenGLRenderWindow.h>
#endif

#if F3D_MODULE_EXTERNAL_RENDERING
#include <vtkExternalOpenGLRenderWindow.h>
#endif

#ifdef _WIN32
#include <Windows.h>
#include <dwmapi.h>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

constexpr auto IMMERSIVE_DARK_MODE_SUPPORTED_SINCE = 19041;
#endif

namespace f3d::detail
{
class window_impl::internals
{
public:
  explicit internals(const options& options)
    : Options(options)
  {
  }

  std::string GetCachePath()
  {
    // create directories if they do not exist
    vtksys::SystemTools::MakeDirectory(this->CachePath);

    return this->CachePath;
  }

#if _WIN32
  /**
   * Helper function to detect if the
   * Windows Build Number is equal or greater to a number
   */
  static bool IsWindowsBuildNumberOrGreater(int buildNumber)
  {
    std::string value{};
    bool result = vtksys::SystemTools::ReadRegistryValue(
      "HKEY_LOCAL_MACHINE\\Software\\Microsoft\\Windows NT\\CurrentVersion;CurrentBuildNumber",
      value);

    if (result == true)
    {
      try
      {
        return std::stoi(value) >= buildNumber;
      }
      catch (const std::invalid_argument& e)
      {
        f3d::log::debug("Error parsing CurrentBuildNumber", e.what());
      }
    }
    else
    {
      f3d::log::debug("Error opening registry key.");
    }

    return false;
  }

  /**
   * Helper function to fetch a DWORD from windows registry.
   *
   * @param hKey A handle to an open registry key
   * @param subKey The path of registry key relative to 'hKey'
   * @param value The name of the registry value
   * @param dWord Variable to store the result in
   */
  static bool ReadRegistryDWord(
    HKEY hKey, const std::wstring& subKey, const std::wstring& value, DWORD& dWord)
  {
    DWORD dataSize = sizeof(DWORD);
    LONG result = RegGetValueW(
      hKey, subKey.c_str(), value.c_str(), RRF_RT_REG_DWORD, nullptr, &dWord, &dataSize);

    return result == ERROR_SUCCESS;
  }

  /**
   * Helper function to detect user theme
   */
  static bool IsWindowsInDarkMode()
  {
    std::wstring subKey(L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize");

    DWORD value{};

    if (ReadRegistryDWord(HKEY_CURRENT_USER, subKey, L"AppsUseLightTheme", value))
    {
      return value == 0;
    }

    if (ReadRegistryDWord(HKEY_CURRENT_USER, subKey, L"SystemUsesLightTheme", value))
    {
      return value == 0;
    }

    return false;
  }
#endif

  void UpdateTheme() const
  {
#ifdef _WIN32
    if (this->IsWindowsBuildNumberOrGreater(IMMERSIVE_DARK_MODE_SUPPORTED_SINCE))
    {
      HWND hwnd = static_cast<HWND>(this->RenWin->GetGenericWindowId());
      BOOL useDarkMode = this->IsWindowsInDarkMode();
      DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDarkMode, sizeof(useDarkMode));
    }
#endif
  }

  static context::fptr SymbolLoader(void* userptr, const char* name)
  {
    assert(userptr != nullptr);
    auto* fn = static_cast<context::function*>(userptr);
    assert(fn != nullptr);
    return (*fn)(name);
  }

  std::unique_ptr<camera_impl> Camera;
  vtkSmartPointer<vtkRenderWindow> RenWin;
  vtkNew<vtkF3DRenderer> Renderer;
  const options& Options;
  std::string CachePath;
  context::function GetProcAddress;
};

//----------------------------------------------------------------------------
window_impl::window_impl(const options& options, const std::optional<Type>& type, bool offscreen,
  const context::function& getProcAddress)
  : Internals(std::make_unique<window_impl::internals>(options))
{
  this->Internals->GetProcAddress = getProcAddress;
  if (type == Type::NONE)
  {
    this->Internals->RenWin = vtkSmartPointer<vtkF3DNoRenderWindow>::New();
  }
  else if (type == Type::EXTERNAL)
  {
#if F3D_MODULE_EXTERNAL_RENDERING
    vtkNew<vtkExternalOpenGLRenderWindow> extWin;
    extWin->AutomaticWindowPositionAndResizeOff();
    this->Internals->RenWin = extWin;
#else
    throw engine::no_window_exception(
      "Window type is external but F3D_MODULE_EXTERNAL_RENDERING is not enabled");
#endif
  }
  else if (type == Type::EGL)
  {
#if defined(VTK_OPENGL_HAS_EGL) && VTK_VERSION_NUMBER >= VTK_VERSION_CHECK(9, 3, 20240914)
    this->Internals->RenWin = vtkSmartPointer<vtkEGLRenderWindow>::New();
#ifdef __ANDROID__
    // Since F3D_MODULE_EXTERNAL_RENDERING is not supported on Android yet, we need to call
    // this workaround. It makes vtkEGLRenderWindow external if WindowInfo is not nullptr.
    this->Internals->RenWin->SetWindowInfo("jni");
#endif
#else
    throw engine::no_window_exception("Window type is EGL but VTK EGL support is not enabled");
#endif
  }
  else if (type == Type::OSMESA)
  {
#if VTK_VERSION_NUMBER >= VTK_VERSION_CHECK(9, 3, 20240914)
    this->Internals->RenWin = vtkSmartPointer<vtkOSOpenGLRenderWindow>::New();
#else
    throw engine::no_window_exception(
      "Window type is OSMesa but VTK OSMesa support is not enabled");
#endif
  }
  else if (type == Type::GLX)
  {
#if defined(VTK_USE_X) && VTK_VERSION_NUMBER >= VTK_VERSION_CHECK(9, 3, 20240914)
    this->Internals->RenWin = vtkSmartPointer<vtkXOpenGLRenderWindow>::New();
#else
    throw engine::no_window_exception("Window type is GLX but VTK GLX support is not enabled");
#endif
  }
  else if (type == Type::WGL)
  {
#ifdef _WIN32
    this->Internals->RenWin = vtkSmartPointer<vtkWin32OpenGLRenderWindow>::New();
#endif
  }
  else if (!type.has_value())
  {
    // rely on VTK logic
    this->Internals->RenWin = vtkSmartPointer<vtkRenderWindow>::New();
  }

  assert(this->Internals->RenWin != nullptr);

#if VTK_VERSION_NUMBER >= VTK_VERSION_CHECK(9, 3, 20240914)
  vtkOpenGLRenderWindow* oglRenWin = vtkOpenGLRenderWindow::SafeDownCast(this->Internals->RenWin);
  if (oglRenWin)
  {
    if (this->Internals->GetProcAddress)
    {
      oglRenWin->SetOpenGLSymbolLoader(&internals::SymbolLoader, &this->Internals->GetProcAddress);
    }
#if F3D_MODULE_EXTERNAL_RENDERING
    if (oglRenWin->IsA("vtkExternalOpenGLRenderWindow"))
    {
      // We need to call vtkOpenGLRenderWindow function because vtkGenericOpenGLRenderWindow is
      // reimplementing it incorrectly
      oglRenWin->vtkOpenGLRenderWindow::OpenGLInit();
    }
#endif
  }
#endif

#if VTK_VERSION_NUMBER >= VTK_VERSION_CHECK(9, 3, 20240606)
  this->Internals->RenWin->EnableTranslucentSurfaceOn();
#endif
  this->Internals->RenWin->SetMultiSamples(0); // Disable hardware antialiasing
  this->Internals->RenWin->SetOffScreenRendering(offscreen);
  this->Internals->RenWin->SetWindowName("f3d");
  this->Internals->RenWin->AddRenderer(this->Internals->Renderer);
  this->Internals->Camera = std::make_unique<detail::camera_impl>();
  this->Internals->Camera->SetVTKRenderer(this->Internals->Renderer);

  this->Initialize();
  this->Internals->UpdateTheme();

  log::debug("VTK window class type is ", this->Internals->RenWin->GetClassName());
}

//----------------------------------------------------------------------------
void window_impl::Initialize()
{
  this->Internals->Renderer->Initialize();
}

//----------------------------------------------------------------------------
void window_impl::InitializeUpVector()
{
  this->Internals->Renderer->InitializeUpVector(this->Internals->Options.scene.up_direction);
}

//----------------------------------------------------------------------------
window_impl::Type window_impl::getType()
{
  if (this->Internals->RenWin->IsA("vtkOSOpenGLRenderWindow"))
  {
    return Type::OSMESA;
  }

#ifdef VTK_USE_X
  if (this->Internals->RenWin->IsA("vtkXOpenGLRenderWindow"))
  {
    return Type::GLX;
  }
#endif

#ifdef _WIN32
  if (this->Internals->RenWin->IsA("vtkWin32OpenGLRenderWindow"))
  {
    return Type::WGL;
  }
#endif

#ifdef __APPLE__
  if (this->Internals->RenWin->IsA("vtkCocoaRenderWindow"))
  {
    return Type::COCOA;
  }
#endif

#ifdef VTK_OPENGL_HAS_EGL
  if (this->Internals->RenWin->IsA("vtkEGLRenderWindow"))
  {
    return Type::EGL;
  }
#endif
#ifdef __EMSCRIPTEN__
  if (this->Internals->RenWin->IsA("vtkWebAssemblyOpenGLRenderWindow"))
  {
    return Type::WASM;
  }
#endif

  if (this->Internals->RenWin->IsA("vtkF3DNoRenderWindow"))
  {
    return Type::NONE;
  }

  return Type::UNKNOWN;
}

//----------------------------------------------------------------------------
bool window_impl::isOffscreen()
{
  return !this->Internals->RenWin->GetShowWindow();
}

//----------------------------------------------------------------------------
camera& window_impl::getCamera()
{
  return *this->Internals->Camera;
}

//----------------------------------------------------------------------------
int window_impl::getWidth() const
{
  return this->Internals->RenWin->GetSize()[0];
}

//----------------------------------------------------------------------------
int window_impl::getHeight() const
{
  return this->Internals->RenWin->GetSize()[1];
}

//----------------------------------------------------------------------------
window& window_impl::setAnimationNameInfo(const std::string& name)
{
  this->Internals->Renderer->SetAnimationnameInfo(name);
  return *this;
}

//----------------------------------------------------------------------------
window& window_impl::setSize(int width, int height)
{
  this->Internals->RenWin->SetSize(width, height);
  return *this;
}

//----------------------------------------------------------------------------
window& window_impl::setPosition(int x, int y)
{
  if (this->Internals->RenWin->IsA("vtkCocoaRenderWindow"))
  {
    // vtkCocoaRenderWindow has a different behavior than other render windows
    // https://gitlab.kitware.com/vtk/vtk/-/issues/18681
    int* screenSize = this->Internals->RenWin->GetScreenSize();
    int* winSize = this->Internals->RenWin->GetSize();
    this->Internals->RenWin->SetPosition(x, screenSize[1] - winSize[1] - y);
  }
  else
  {
    this->Internals->RenWin->SetPosition(x, y);
  }
  return *this;
}

//----------------------------------------------------------------------------
window& window_impl::setIcon(const unsigned char* icon, size_t iconSize)
{
  // XXX This code requires that the interactor has already been set on the render window
  vtkNew<vtkPNGReader> iconReader;
  iconReader->SetMemoryBuffer(icon);
  iconReader->SetMemoryBufferLength(iconSize);
  iconReader->Update();
  this->Internals->RenWin->SetIcon(iconReader->GetOutput());
  return *this;
}

//----------------------------------------------------------------------------
window& window_impl::setWindowName(const std::string& windowName)
{
  this->Internals->RenWin->SetWindowName(windowName.c_str());
  return *this;
}

//----------------------------------------------------------------------------
point3_t window_impl::getWorldFromDisplay(const point3_t& displayPoint) const
{
  point3_t out = { 0.0, 0.0, 0.0 };
  double worldPt[4];
  this->Internals->Renderer->SetDisplayPoint(displayPoint.data());
  this->Internals->Renderer->DisplayToWorld();
  this->Internals->Renderer->GetWorldPoint(worldPt);

  constexpr double homogeneousThreshold = 1e-7;
  if (worldPt[3] > homogeneousThreshold)
  {
    out[0] = worldPt[0] / worldPt[3];
    out[1] = worldPt[1] / worldPt[3];
    out[2] = worldPt[2] / worldPt[3];
  }
  return out;
}

//----------------------------------------------------------------------------
point3_t window_impl::getDisplayFromWorld(const point3_t& worldPoint) const
{
  point3_t out;
  this->Internals->Renderer->SetWorldPoint(worldPoint[0], worldPoint[1], worldPoint[2], 1.0);
  this->Internals->Renderer->WorldToDisplay();
  this->Internals->Renderer->GetDisplayPoint(out.data());
  return out;
}

//----------------------------------------------------------------------------
window_impl::~window_impl()
{
  // The axis widget should be disabled before calling the renderer destructor
  // As there is a register loop if not
  this->Internals->Renderer->ShowAxis(false);
}

//----------------------------------------------------------------------------
void window_impl::UpdateDynamicOptions()
{
  vtkF3DRenderer* renderer = this->Internals->Renderer;

  if (this->Internals->RenWin->IsA("vtkF3DNoRenderWindow"))
  {
    // With a NONE window type, only update the actors to get accurate bounding box information
    renderer->UpdateActors();
    return;
  }

  // Set the cache path if not already
  renderer->SetCachePath(this->Internals->GetCachePath());

  // Make sure lights are created before we take options into account
  renderer->UpdateLights();

  const options& opt = this->Internals->Options;
  renderer->ShowAxis(opt.interactor.axis);
  renderer->SetUseTrackball(opt.interactor.trackball);
  renderer->SetInvertZoom(opt.interactor.invert_zoom);

  // XXX: model.point_sprites.type only has an effect on geometry scene
  // but we set it here for practical reasons
  const int pointSpritesSize = opt.model.point_sprites.size;
  const vtkF3DRenderer::SplatType splatType = opt.model.point_sprites.type == "gaussian"
    ? vtkF3DRenderer::SplatType::GAUSSIAN
    : vtkF3DRenderer::SplatType::SPHERE;
  renderer->SetPointSpritesProperties(splatType, pointSpritesSize);

  renderer->SetLineWidth(opt.render.line_width);
  renderer->SetPointSize(opt.render.point_size);
  renderer->ShowEdge(opt.render.show_edges);
  renderer->ShowTimer(opt.ui.fps);
  renderer->ShowFilename(opt.ui.filename);
  renderer->SetFilenameInfo(opt.ui.filename_info);
  renderer->ShowMetaData(opt.ui.metadata);
  renderer->ShowCheatSheet(opt.ui.cheatsheet);
  renderer->ShowDropZone(opt.ui.dropzone);
  renderer->SetDropZoneInfo(opt.ui.dropzone_info);

  renderer->SetUseRaytracing(opt.render.raytracing.enable);
  renderer->SetRaytracingSamples(opt.render.raytracing.samples);
  renderer->SetUseRaytracingDenoiser(opt.render.raytracing.denoise);

  renderer->SetUseSSAOPass(opt.render.effect.ambient_occlusion);
  renderer->SetUseFXAAPass(opt.render.effect.anti_aliasing);
  renderer->SetUseToneMappingPass(opt.render.effect.tone_mapping);
  renderer->SetUseDepthPeelingPass(opt.render.effect.translucency_support);
  renderer->SetBackfaceType(opt.render.backface_type);
  renderer->SetFinalShader(opt.render.effect.final_shader);

  renderer->SetBackground(opt.render.background.color.data());
  renderer->SetUseBlurBackground(opt.render.background.blur);
  renderer->SetBlurCircleOfConfusionRadius(opt.render.background.blur_coc);
  renderer->SetLightIntensity(opt.render.light.intensity);

  renderer->SetHDRIFile(opt.render.hdri.file);
  renderer->SetUseImageBasedLighting(opt.render.hdri.ambient);
  renderer->ShowHDRISkybox(opt.render.background.skybox);

  renderer->SetFontFile(opt.ui.font_file);

  renderer->SetGridUnitSquare(opt.render.grid.unit);
  renderer->SetGridSubdivisions(opt.render.grid.subdivisions);
  renderer->SetGridAbsolute(opt.render.grid.absolute);
  renderer->ShowGrid(opt.render.grid.enable);
  renderer->SetGridColor(opt.render.grid.color);

  if (!opt.scene.camera.index.has_value())
  {
    renderer->SetUseOrthographicProjection(opt.scene.camera.orthographic);
  }

  renderer->SetSurfaceColor(opt.model.color.rgb);
  renderer->SetOpacity(opt.model.color.opacity);
  renderer->SetTextureBaseColor(opt.model.color.texture);
  renderer->SetRoughness(opt.model.material.roughness);
  renderer->SetMetallic(opt.model.material.metallic);
  renderer->SetTextureMaterial(opt.model.material.texture);
  renderer->SetTextureEmissive(opt.model.emissive.texture);
  renderer->SetEmissiveFactor(opt.model.emissive.factor);
  renderer->SetTextureNormal(opt.model.normal.texture);
  renderer->SetNormalScale(opt.model.normal.scale);
  renderer->SetTextureMatCap(opt.model.matcap.texture);

  renderer->SetEnableColoring(opt.model.scivis.enable);
  renderer->SetUseCellColoring(opt.model.scivis.cells);
  renderer->SetArrayNameForColoring(opt.model.scivis.array_name);
  renderer->SetComponentForColoring(opt.model.scivis.component);

  renderer->SetScalarBarRange(opt.model.scivis.range);
  renderer->SetColormap(opt.model.scivis.colormap);
  renderer->ShowScalarBar(opt.ui.scalar_bar);

  renderer->SetUsePointSprites(opt.model.point_sprites.enable);
  renderer->SetUseVolume(opt.model.volume.enable);
  renderer->SetUseInverseOpacityFunction(opt.model.volume.inverse);

  renderer->UpdateActors();
}

//----------------------------------------------------------------------------
void window_impl::PrintSceneDescription(log::VerboseLevel level)
{
  log::print(level, this->Internals->Renderer->GetSceneDescription());
}

//----------------------------------------------------------------------------
void window_impl::PrintColoringDescription(log::VerboseLevel level)
{
  std::string descr = this->Internals->Renderer->GetColoringDescription();
  if (!descr.empty())
  {
    log::print(level, descr);
  }
}

//----------------------------------------------------------------------------
vtkRenderWindow* window_impl::GetRenderWindow()
{
  return this->Internals->RenWin;
}

//----------------------------------------------------------------------------
bool window_impl::render()
{
  this->UpdateDynamicOptions();
  this->Internals->RenWin->Render();
  return true;
}

//----------------------------------------------------------------------------
image window_impl::renderToImage(bool noBackground)
{
  this->UpdateDynamicOptions();

  vtkNew<vtkWindowToImageFilter> rtW2if;
  rtW2if->SetInput(this->Internals->RenWin);

  if (noBackground)
  {
    // we need to set the background to black to avoid blending issues with translucent
    // objects when saving to file with no background
    this->Internals->RenWin->GetRenderers()->GetFirstRenderer()->SetBackground(0, 0, 0);
    rtW2if->SetInputBufferTypeToRGBA();
  }

  vtkNew<vtkImageExport> exporter;
  exporter->SetInputConnection(rtW2if->GetOutputPort());
  exporter->ImageLowerLeftOn();

  int* dims = exporter->GetDataDimensions();
  int cmp = exporter->GetDataNumberOfScalarComponents();

  image output(dims[0], dims[1], cmp);
  exporter->Export(output.getContent());

  return output;
}

//----------------------------------------------------------------------------
void window_impl::SetImporter(vtkF3DMetaImporter* importer)
{
  this->Internals->Renderer->SetImporter(importer);
}

//----------------------------------------------------------------------------
void window_impl::SetCachePath(const std::string& cachePath)
{
  this->Internals->CachePath = cachePath;
}
};
