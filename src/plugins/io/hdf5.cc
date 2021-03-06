#include <hdf5.h>
#include <unistd.h>
#include <errno.h>
#include <pressio_data.h>
#include <cassert>
#include <vector>
#include <string>
#include <pressio_compressor.h>
#include <libpressio_ext/compat/std_compat.h>
#include "libpressio_ext/io/posix.h"
#include "libpressio_ext/cpp/pressio.h"
#include "libpressio_ext/cpp/options.h"
#include "libpressio_ext/cpp/data.h"
#include "libpressio_ext/cpp/io.h"

namespace {
  compat::optional<pressio_dtype> h5t_to_pressio(hid_t h5type) {
    if(H5Tequal(h5type, H5T_NATIVE_INT8) > 0) return pressio_int8_dtype;
    if(H5Tequal(h5type, H5T_NATIVE_INT16) > 0) return pressio_int16_dtype;
    if(H5Tequal(h5type, H5T_NATIVE_INT32) > 0) return pressio_int32_dtype;
    if(H5Tequal(h5type, H5T_NATIVE_INT64) > 0) return pressio_int64_dtype;
    if(H5Tequal(h5type, H5T_NATIVE_UINT8) > 0) return pressio_uint8_dtype;
    if(H5Tequal(h5type, H5T_NATIVE_UINT16) > 0) return pressio_uint16_dtype;
    if(H5Tequal(h5type, H5T_NATIVE_UINT32) > 0) return pressio_uint32_dtype;
    if(H5Tequal(h5type, H5T_NATIVE_UINT64) > 0) return pressio_uint64_dtype;
    if(H5Tequal(h5type, H5T_NATIVE_FLOAT) > 0) return pressio_float_dtype;
    if(H5Tequal(h5type, H5T_NATIVE_DOUBLE) > 0) return pressio_double_dtype;
    return compat::optional<pressio_dtype>{};
  }
  hid_t pressio_to_h5t(pressio_dtype dtype) {
    switch(dtype) {
      case pressio_double_dtype: 
        return H5T_NATIVE_DOUBLE;
      case pressio_float_dtype:
        return H5T_NATIVE_FLOAT;
      case pressio_uint8_dtype:
        return H5T_NATIVE_UINT8;
      case pressio_uint16_dtype:
        return H5T_NATIVE_UINT16;
      case pressio_uint32_dtype:
        return H5T_NATIVE_UINT32;
      case pressio_uint64_dtype:
        return H5T_NATIVE_UINT64;
      case pressio_int8_dtype:
        return H5T_NATIVE_INT8;
      case pressio_int16_dtype:
        return H5T_NATIVE_INT16;
      case pressio_int32_dtype:
        return H5T_NATIVE_INT32;
      case pressio_int64_dtype:
        return H5T_NATIVE_INT64;
      case pressio_byte_dtype:
        return H5T_NATIVE_UCHAR;
      default:
        assert(false && "unexpected type");
        //shutup gcc
        return H5T_NATIVE_UCHAR;
    }
  }

  bool hdf_path_exists(hid_t file, std::string const& path) {
    if(path == std::string("") or path == std::string("/")) return true;
    else {
      //check for parent path
      auto last_slash_pos = path.find_last_of('/');
      if(last_slash_pos != std::string::npos)
      {
        //recurse to check for parent
        auto parent = path.substr(0, last_slash_pos - 1);
        if (not hdf_path_exists(file, parent)) return false;
      } 

      //check the path passed in
      return H5Lexists(file, path.c_str(), H5P_DEFAULT);
    }
  }

  /**
   * this class is a standard c++ idiom for closing resources
   * it calls the function passed in during the destructor.
   */
  template <class Function>
  class cleanup {
    public:
      cleanup(Function f) noexcept: cleanup_fn(std::move(f)), do_cleanup(true) {}
      cleanup(cleanup&& rhs) noexcept: cleanup_fn(std::move(rhs.cleanup_fn)), do_cleanup(true) {
        do_cleanup = false;
      }
      cleanup(cleanup const&)=delete;
      cleanup& operator=(cleanup const&)=delete;
      cleanup& operator=(cleanup && rhs) noexcept { 
        do_cleanup = compat::exchange(rhs.do_cleanup, false);
        cleanup_fn = std::move(rhs.cleanup_fn);
      }
      ~cleanup() { if(do_cleanup) cleanup_fn(); }

    private:
      Function cleanup_fn;
      bool do_cleanup;
  };
  template<class Function>
  auto make_cleanup(Function&& f) -> decltype(cleanup<Function>(std::declval<Function>())) {
    return cleanup<Function>(std::forward<Function>(f));
  }
}

extern "C" {

struct pressio_data*
pressio_io_data_path_h5read(const char* file_name, const char* dataset_name)
{
  hid_t file = H5Fopen(file_name, H5F_ACC_RDONLY, H5P_DEFAULT);
  if(file < 0) return nullptr;
  auto cleanup_file = make_cleanup([&]{ H5Fclose(file); });

  hid_t dataset = H5Dopen2(file, dataset_name, H5P_DEFAULT);
  if(dataset < 0) return nullptr;
  auto cleanup_dataset = make_cleanup([&]{ H5Dclose(dataset); });

  hid_t dataspace = H5Dget_space(dataset);
  if(dataspace < 0) return nullptr;
  auto cleanup_dataspace = make_cleanup([&]{ H5Sclose(dataspace); });

  int ndims = H5Sget_simple_extent_ndims(dataspace);
  std::vector<hsize_t> dims(ndims);
  H5Sget_simple_extent_dims(dataspace, dims.data(), nullptr);

  //convert to size_t from hsize_t
  std::vector<size_t> pressio_dims(std::begin(dims), std::end(dims));

  hid_t type = H5Dget_type(dataset);
  if(type < 0) return nullptr;
  auto cleanup_type = make_cleanup([&]{ H5Tclose(type);}) ;

  {
    auto dtype = h5t_to_pressio(type);
    if( dtype) {
      auto ret = pressio_data_new_owning(*dtype, pressio_dims.size(), pressio_dims.data());
      auto ptr = pressio_data_ptr(ret, nullptr);

      H5Dread(dataset, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, ptr);

      return ret;
    } else {
      return nullptr;
    }
  }
}

int
pressio_io_data_path_h5write(struct pressio_data const* data, const char* file_name, const char* dataset_name)
{
  //check if the file exists
  hid_t file;
  int perms_ok = access(file_name, W_OK);
  if(perms_ok == 0)
  {
    file = H5Fopen(file_name, H5F_ACC_RDWR, H5P_DEFAULT);
  } else {
    if(errno == ENOENT) {
      file = H5Fcreate(file_name, H5F_ACC_EXCL, H5P_DEFAULT, H5P_DEFAULT);
    } else {
      return 1;
    }
  }
  if(file < 0) return 1;
  auto cleanup_file = make_cleanup([&]{ H5Fclose(file); });


  //prepare the dataset for writing
  std::vector<size_t> dims;
  for (size_t i = 0; i < pressio_data_num_dimensions(data); ++i) {
    dims.push_back(pressio_data_get_dimension(data, i));
  }

  std::vector<hsize_t> h5_dims(std::begin(dims), std::end(dims));
  hid_t dataspace = H5Screate_simple(
      h5_dims.size(),
      h5_dims.data(),
      nullptr
      );
  if(dataspace < 0) return 1;
  auto cleanup_space = make_cleanup([&]{ H5Sclose(dataspace);});

  hid_t dataset;
  if (hdf_path_exists(file, dataset_name))
  {
    dataset = H5Dopen(file, dataset_name, H5P_DEFAULT);
  } else {
    dataset = H5Dcreate2(file,
        dataset_name,
        pressio_to_h5t(pressio_data_dtype(data)),
        dataspace,
        H5P_DEFAULT,
        H5P_DEFAULT,
        H5P_DEFAULT
        );
  }
  if(dataset < 0) return 1;
  auto cleanup_dataset = make_cleanup([&]{ H5Dclose(dataset);});

  //write the dataset
  return (H5Dwrite(
      dataset,
      pressio_to_h5t(pressio_data_dtype(data)),
      H5S_ALL,
      H5S_ALL,
      H5P_DEFAULT,
      pressio_data_ptr(data,nullptr)
      ) < 0);
}

}

struct hdf5_io: public libpressio_io_plugin {
  virtual struct pressio_data* read_impl(struct pressio_data*) override {
    return pressio_io_data_path_h5read(filename.c_str(), dataset.c_str());
  }

  virtual int write_impl(struct pressio_data const* data) override{
    return pressio_io_data_path_h5write(data, filename.c_str(), dataset.c_str());
  }

  virtual struct pressio_options get_configuration_impl() const override{
    return {
      {"pressio:thread_safe",  static_cast<int>(pressio_thread_safety_single)}
    };
  }

  virtual int set_options_impl(struct pressio_options const& options) override{
    get(options, "io:path", &filename);
    get(options, "hdf5:dataset", &dataset);
    return 0;
  }
  virtual struct pressio_options get_options_impl() const override{
    pressio_options opts;
    set(opts, "io:path", filename);
    set(opts, "hdf5:dataset", dataset);
    return opts;
  }

  int patch_version() const override{ 
    return 1;
  }
  virtual const char* version() const override{
    return "0.0.1";
  }

  const char* prefix() const override {
    return "hdf5";
  }

  std::shared_ptr<libpressio_io_plugin> clone() override {
    return compat::make_unique<hdf5_io>(*this);
  }

  private:
  std::string filename;
  std::string dataset;
};

static pressio_register X(io_plugins(), "hdf5", [](){ return compat::make_unique<hdf5_io>(); });
