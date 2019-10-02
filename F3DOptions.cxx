#include "F3DOptions.h"

#include "vtkF3DGenericImporter.h"

#include <vtk_jsoncpp.h>

#include <fstream>

//----------------------------------------------------------------------------
bool F3DOptions::InitializeFromArgs(int argc, char** argv)
{
  try
  {
    cxxopts::Options options(argv[0], f3d::AppTitle);
    options
      .positional_help("input_file")
      .show_positional_help();

    options
      .add_options()
      ("input", "Input file", cxxopts::value<std::string>(), "file")
      ("h,help", "Print help")
      ("v,verbose", "Enable verbose mode", cxxopts::value<bool>(this->Verbose))
      ("x,axis", "Show axis", cxxopts::value<bool>(this->Axis))
      ("g,grid", "Show grid", cxxopts::value<bool>(this->Grid))
      ("n,normals", "Show mesh normals", cxxopts::value<bool>(this->Normals));

    options
      .add_options("Window")
      ("bg-color", "Background color", cxxopts::value<std::vector<double>>(this->BackgroundColor)->default_value("0.2,0.2,0.2"))
      ("resolution", "Window resolution", cxxopts::value<std::vector<int>>(this->WindowSize)->default_value("1000,600"));

    options
      .add_options("Scientific visualization")
      ("scalars", "Color by scalars", cxxopts::value<std::string>(this->Scalars)->implicit_value("f3d_reserved"), "array_name")
      ("comp", "Specify the component used", cxxopts::value<int>(this->Component), "comp_index")
      ("cells", "The array is located on cells", cxxopts::value<bool>(this->Cells))
      ("range", "Custom range for the array", cxxopts::value<std::vector<double>>(this->Range), "min,max")
      ("b,hide-bar", "Hide scalar bar", cxxopts::value<bool>(this->HideBar));

    options
      .add_options("PostFX")
      ("d,depth-peeling", "Enable depth peeling", cxxopts::value<bool>(this->DepthPeeling))
      ("f,fxaa", "Enable FXAA anti-aliasing", cxxopts::value<bool>(this->FXAA))
      ("u,ssao", "Enable Screen-Space Ambient Occlusion", cxxopts::value<bool>(this->SSAO));

    options.parse_positional({"input", "positional"});

    if (argc == 1)
    {
      std::cout << options.help() << std::endl;
      exit(EXIT_FAILURE);
    }

    auto result = options.parse(argc, argv);

    if (result.count("help") > 0)
    {
      std::cout << options.help() << std::endl;
      exit(EXIT_SUCCESS);
    }

    this->Input = result["input"].as<std::string>().c_str();
  }
  catch (const cxxopts::OptionException &e)
  {
    std::cout << "error parsing options: " << e.what() << std::endl;
    exit(EXIT_FAILURE);
  }
  return true;
}

//----------------------------------------------------------------------------
bool F3DOptions::InitializeFromFile(const std::string &fname)
{
  std::ifstream file;
  file.open(fname.c_str());

  if (!file.is_open())
  {
    std::cerr << "Unable to open configuration file " << fname << endl;
    return false;
  }
  Json::CharReaderBuilder builder;
  builder["collectComments"] = false;
  Json::Value root;
  std::string errs;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  bool success = Json::parseFromStream(builder, file, &root, &errs);
  if (!success)
  {
    std::cerr << "Unable to parse the configuration file " << fname << endl;
    return false;
  }

  // TODO

  return true;
}