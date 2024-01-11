package=native_libmultiprocess
$(package)_version=2cbbd09d8b9972a8f5c68b510c0dae9a9f7a22da
$(package)_download_path=https://github.com/chaincodelabs/libmultiprocess/archive
$(package)_file_name=$($(package)_version).tar.gz
$(package)_sha256_hash=a86416908cc4d27a41c58d846ca854730b1c56bd7a58d823348969e300a10014
$(package)_dependencies=native_capnp

define $(package)_config_cmds
  $($(package)_cmake) .
endef

define $(package)_build_cmds
  $(MAKE)
endef

define $(package)_stage_cmds
  $(MAKE) DESTDIR=$($(package)_staging_dir) install-bin
endef
