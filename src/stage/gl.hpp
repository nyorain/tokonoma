namespace doi {

enum GLInternalFormat {
    GL_DEPTH_COMPONENT                                            = 0x1902, // decimal value: 6402
    GL_RED                                                        = 0x1903, // decimal value: 6403
    GL_RGB                                                        = 0x1907, // decimal value: 6407
    GL_RGBA                                                       = 0x1908, // decimal value: 6408
    GL_R3_G3_B2                                                   = 0x2A10, // decimal value: 10768
    GL_ALPHA4                                                     = 0x803B, // decimal value: 32827
    GL_ALPHA8                                                     = 0x803C, // decimal value: 32828
    GL_ALPHA12                                                    = 0x803D, // decimal value: 32829
    GL_ALPHA16                                                    = 0x803E, // decimal value: 32830
    GL_LUMINANCE4                                                 = 0x803F, // decimal value: 32831
    GL_LUMINANCE8                                                 = 0x8040, // decimal value: 32832
    GL_LUMINANCE12                                                = 0x8041, // decimal value: 32833
    GL_LUMINANCE16                                                = 0x8042, // decimal value: 32834
    GL_LUMINANCE4_ALPHA4                                          = 0x8043, // decimal value: 32835
    GL_LUMINANCE6_ALPHA2                                          = 0x8044, // decimal value: 32836
    GL_LUMINANCE8_ALPHA8                                          = 0x8045, // decimal value: 32837
    GL_LUMINANCE12_ALPHA4                                         = 0x8046, // decimal value: 32838
    GL_LUMINANCE12_ALPHA12                                        = 0x8047, // decimal value: 32839
    GL_LUMINANCE16_ALPHA16                                        = 0x8048, // decimal value: 32840
    GL_INTENSITY                                                  = 0x8049, // decimal value: 32841
    GL_INTENSITY4                                                 = 0x804A, // decimal value: 32842
    GL_INTENSITY8                                                 = 0x804B, // decimal value: 32843
    GL_INTENSITY12                                                = 0x804C, // decimal value: 32844
    GL_INTENSITY16                                                = 0x804D, // decimal value: 32845
    GL_RGB2_EXT                                                   = 0x804E, // decimal value: 32846
    GL_RGB4                                                       = 0x804F, // decimal value: 32847
    GL_RGB4_EXT                                                   = 0x804F, // decimal value: 32847
    GL_RGB5                                                       = 0x8050, // decimal value: 32848
    GL_RGB5_EXT                                                   = 0x8050, // decimal value: 32848
    GL_RGB8                                                       = 0x8051, // decimal value: 32849
    GL_RGB8_EXT                                                   = 0x8051, // decimal value: 32849
    GL_RGB10                                                      = 0x8052, // decimal value: 32850
    GL_RGB10_EXT                                                  = 0x8052, // decimal value: 32850
    GL_RGB12                                                      = 0x8053, // decimal value: 32851
    GL_RGB12_EXT                                                  = 0x8053, // decimal value: 32851
    GL_RGB16                                                      = 0x8054, // decimal value: 32852
    GL_RGB16_EXT                                                  = 0x8054, // decimal value: 32852
    GL_RGBA4                                                      = 0x8056, // decimal value: 32854
    GL_RGBA4_EXT                                                  = 0x8056, // decimal value: 32854
    GL_RGB5_A1                                                    = 0x8057, // decimal value: 32855
    GL_RGB5_A1_EXT                                                = 0x8057, // decimal value: 32855
    GL_RGBA8                                                      = 0x8058, // decimal value: 32856
    GL_RGBA8_EXT                                                  = 0x8058, // decimal value: 32856
    GL_RGB10_A2                                                   = 0x8059, // decimal value: 32857
    GL_RGB10_A2_EXT                                               = 0x8059, // decimal value: 32857
    GL_RGBA12                                                     = 0x805A, // decimal value: 32858
    GL_RGBA12_EXT                                                 = 0x805A, // decimal value: 32858
    GL_RGBA16                                                     = 0x805B, // decimal value: 32859
    GL_RGBA16_EXT                                                 = 0x805B, // decimal value: 32859
    GL_DUAL_ALPHA4_SGIS                                           = 0x8110, // decimal value: 33040
    GL_DUAL_ALPHA8_SGIS                                           = 0x8111, // decimal value: 33041
    GL_DUAL_ALPHA12_SGIS                                          = 0x8112, // decimal value: 33042
    GL_DUAL_ALPHA16_SGIS                                          = 0x8113, // decimal value: 33043
    GL_DUAL_LUMINANCE4_SGIS                                       = 0x8114, // decimal value: 33044
    GL_DUAL_LUMINANCE8_SGIS                                       = 0x8115, // decimal value: 33045
    GL_DUAL_LUMINANCE12_SGIS                                      = 0x8116, // decimal value: 33046
    GL_DUAL_LUMINANCE16_SGIS                                      = 0x8117, // decimal value: 33047
    GL_DUAL_INTENSITY4_SGIS                                       = 0x8118, // decimal value: 33048
    GL_DUAL_INTENSITY8_SGIS                                       = 0x8119, // decimal value: 33049
    GL_DUAL_INTENSITY12_SGIS                                      = 0x811A, // decimal value: 33050
    GL_DUAL_INTENSITY16_SGIS                                      = 0x811B, // decimal value: 33051
    GL_DUAL_LUMINANCE_ALPHA4_SGIS                                 = 0x811C, // decimal value: 33052
    GL_DUAL_LUMINANCE_ALPHA8_SGIS                                 = 0x811D, // decimal value: 33053
    GL_QUAD_ALPHA4_SGIS                                           = 0x811E, // decimal value: 33054
    GL_QUAD_ALPHA8_SGIS                                           = 0x811F, // decimal value: 33055
    GL_QUAD_LUMINANCE4_SGIS                                       = 0x8120, // decimal value: 33056
    GL_QUAD_LUMINANCE8_SGIS                                       = 0x8121, // decimal value: 33057
    GL_QUAD_INTENSITY4_SGIS                                       = 0x8122, // decimal value: 33058
    GL_QUAD_INTENSITY8_SGIS                                       = 0x8123, // decimal value: 33059
    GL_DEPTH_COMPONENT16                                          = 0x81A5, // decimal value: 33189
    GL_DEPTH_COMPONENT16_ARB                                      = 0x81A5, // decimal value: 33189
    GL_DEPTH_COMPONENT16_SGIX                                     = 0x81A5, // decimal value: 33189
    GL_DEPTH_COMPONENT24_ARB                                      = 0x81A6, // decimal value: 33190
    GL_DEPTH_COMPONENT24_SGIX                                     = 0x81A6, // decimal value: 33190
    GL_DEPTH_COMPONENT32_ARB                                      = 0x81A7, // decimal value: 33191
    GL_DEPTH_COMPONENT32_SGIX                                     = 0x81A7, // decimal value: 33191
    GL_COMPRESSED_RED                                             = 0x8225, // decimal value: 33317
    GL_COMPRESSED_RG                                              = 0x8226, // decimal value: 33318
    GL_RG                                                         = 0x8227, // decimal value: 33319
    GL_R8                                                         = 0x8229, // decimal value: 33321
    GL_R16                                                        = 0x822A, // decimal value: 33322
    GL_RG8                                                        = 0x822B, // decimal value: 33323
    GL_RG16                                                       = 0x822C, // decimal value: 33324
    GL_R16F                                                       = 0x822D, // decimal value: 33325
    GL_R32F                                                       = 0x822E, // decimal value: 33326
    GL_RG16F                                                      = 0x822F, // decimal value: 33327
    GL_RG32F                                                      = 0x8230, // decimal value: 33328
    GL_R8I                                                        = 0x8231, // decimal value: 33329
    GL_R8UI                                                       = 0x8232, // decimal value: 33330
    GL_R16I                                                       = 0x8233, // decimal value: 33331
    GL_R16UI                                                      = 0x8234, // decimal value: 33332
    GL_R32I                                                       = 0x8235, // decimal value: 33333
    GL_R32UI                                                      = 0x8236, // decimal value: 33334
    GL_RG8I                                                       = 0x8237, // decimal value: 33335
    GL_RG8UI                                                      = 0x8238, // decimal value: 33336
    GL_RG16I                                                      = 0x8239, // decimal value: 33337
    GL_RG16UI                                                     = 0x823A, // decimal value: 33338
    GL_RG32I                                                      = 0x823B, // decimal value: 33339
    GL_RG32UI                                                     = 0x823C, // decimal value: 33340
    GL_COMPRESSED_RGB_S3TC_DXT1_EXT                               = 0x83F0, // decimal value: 33776
    GL_COMPRESSED_RGBA_S3TC_DXT1_EXT                              = 0x83F1, // decimal value: 33777
    GL_COMPRESSED_RGBA_S3TC_DXT3_EXT                              = 0x83F2, // decimal value: 33778
    GL_COMPRESSED_RGBA_S3TC_DXT5_EXT                              = 0x83F3, // decimal value: 33779
    GL_COMPRESSED_RGB                                             = 0x84ED, // decimal value: 34029
    GL_COMPRESSED_RGBA                                            = 0x84EE, // decimal value: 34030
    GL_DEPTH_STENCIL                                              = 0x84F9, // decimal value: 34041
    GL_DEPTH_STENCIL_EXT                                          = 0x84F9, // decimal value: 34041
    GL_DEPTH_STENCIL_NV                                           = 0x84F9, // decimal value: 34041
    GL_RGBA32F                                                    = 0x8814, // decimal value: 34836
    GL_RGBA32F_ARB                                                = 0x8814, // decimal value: 34836
    GL_RGBA16F                                                    = 0x881A, // decimal value: 34842
    GL_RGBA16F_ARB                                                = 0x881A, // decimal value: 34842
    GL_RGB16F                                                     = 0x881B, // decimal value: 34843
    GL_RGB16F_ARB                                                 = 0x881B, // decimal value: 34843
    GL_DEPTH24_STENCIL8                                           = 0x88F0, // decimal value: 35056
    GL_DEPTH24_STENCIL8_EXT                                       = 0x88F0, // decimal value: 35056
    GL_R11F_G11F_B10F                                             = 0x8C3A, // decimal value: 35898
    GL_R11F_G11F_B10F_EXT                                         = 0x8C3A, // decimal value: 35898
    GL_RGB9_E5                                                    = 0x8C3D, // decimal value: 35901
    GL_RGB9_E5_EXT                                                = 0x8C3D, // decimal value: 35901
    GL_SRGB                                                       = 0x8C40, // decimal value: 35904
    GL_SRGB_EXT                                                   = 0x8C40, // decimal value: 35904
    GL_SRGB8                                                      = 0x8C41, // decimal value: 35905
    GL_SRGB8_EXT                                                  = 0x8C41, // decimal value: 35905
    GL_SRGB_ALPHA                                                 = 0x8C42, // decimal value: 35906
    GL_SRGB_ALPHA_EXT                                             = 0x8C42, // decimal value: 35906
    GL_SRGB8_ALPHA8                                               = 0x8C43, // decimal value: 35907
    GL_SRGB8_ALPHA8_EXT                                           = 0x8C43, // decimal value: 35907
    GL_COMPRESSED_SRGB                                            = 0x8C48, // decimal value: 35912
    GL_COMPRESSED_SRGB_ALPHA                                      = 0x8C49, // decimal value: 35913
    GL_COMPRESSED_SRGB_S3TC_DXT1_EXT                              = 0x8C4C, // decimal value: 35916
    GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT                        = 0x8C4D, // decimal value: 35917
    GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT                        = 0x8C4E, // decimal value: 35918
    GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT                        = 0x8C4F, // decimal value: 35919
    GL_DEPTH_COMPONENT32F                                         = 0x8CAC, // decimal value: 36012
    GL_DEPTH32F_STENCIL8                                          = 0x8CAD, // decimal value: 36013
    GL_RGBA32UI                                                   = 0x8D70, // decimal value: 36208
    GL_RGB32UI                                                    = 0x8D71, // decimal value: 36209
    GL_RGBA16UI                                                   = 0x8D76, // decimal value: 36214
    GL_RGB16UI                                                    = 0x8D77, // decimal value: 36215
    GL_RGBA8UI                                                    = 0x8D7C, // decimal value: 36220
    GL_RGB8UI                                                     = 0x8D7D, // decimal value: 36221
    GL_RGBA32I                                                    = 0x8D82, // decimal value: 36226
    GL_RGB32I                                                     = 0x8D83, // decimal value: 36227
    GL_RGBA16I                                                    = 0x8D88, // decimal value: 36232
    GL_RGB16I                                                     = 0x8D89, // decimal value: 36233
    GL_RGBA8I                                                     = 0x8D8E, // decimal value: 36238
    GL_RGB8I                                                      = 0x8D8F, // decimal value: 36239
    GL_DEPTH_COMPONENT32F_NV                                      = 0x8DAB, // decimal value: 36267
    GL_DEPTH32F_STENCIL8_NV                                       = 0x8DAC, // decimal value: 36268
    GL_COMPRESSED_RED_RGTC1                                       = 0x8DBB, // decimal value: 36283
    GL_COMPRESSED_RED_RGTC1_EXT                                   = 0x8DBB, // decimal value: 36283
    GL_COMPRESSED_SIGNED_RED_RGTC1                                = 0x8DBC, // decimal value: 36284
    GL_COMPRESSED_SIGNED_RED_RGTC1_EXT                            = 0x8DBC, // decimal value: 36284
    GL_COMPRESSED_RG_RGTC2                                        = 0x8DBD, // decimal value: 36285
    GL_COMPRESSED_SIGNED_RG_RGTC2                                 = 0x8DBE, // decimal value: 36286
    GL_COMPRESSED_RGBA_BPTC_UNORM                                 = 0x8E8C, // decimal value: 36492
    GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM                           = 0x8E8D, // decimal value: 36493
    GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT                           = 0x8E8E, // decimal value: 36494
    GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT                         = 0x8E8F, // decimal value: 36495
    GL_R8_SNORM                                                   = 0x8F94, // decimal value: 36756
    GL_RG8_SNORM                                                  = 0x8F95, // decimal value: 36757
    GL_RGB8_SNORM                                                 = 0x8F96, // decimal value: 36758
    GL_RGBA8_SNORM                                                = 0x8F97, // decimal value: 36759
    GL_R16_SNORM                                                  = 0x8F98, // decimal value: 36760
    GL_RG16_SNORM                                                 = 0x8F99, // decimal value: 36761
    GL_RGB16_SNORM                                                = 0x8F9A, // decimal value: 36762
    GL_RGB10_A2UI                                                 = 0x906F, // decimal value: 36975
    GL_COMPRESSED_R11_EAC                                         = 0x9270, // decimal value: 37488
    GL_COMPRESSED_SIGNED_R11_EAC                                  = 0x9271, // decimal value: 37489
    GL_COMPRESSED_RG11_EAC                                        = 0x9272, // decimal value: 37490
    GL_COMPRESSED_SIGNED_RG11_EAC                                 = 0x9273, // decimal value: 37491
    GL_COMPRESSED_RGB8_ETC2                                       = 0x9274, // decimal value: 37492
    GL_COMPRESSED_SRGB8_ETC2                                      = 0x9275, // decimal value: 37493
    GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2                   = 0x9276, // decimal value: 37494
    GL_COMPRESSED_SRGB8_PUNCHTHROUGH_ALPHA1_ETC2                  = 0x9277, // decimal value: 37495
    GL_COMPRESSED_RGBA8_ETC2_EAC                                  = 0x9278, // decimal value: 37496
    GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC                           = 0x9279, // decimal value: 37497
	GL_SR8                                                    	  = 0x8FBD, // decimal value: 36797
};

enum GLPixelFormat {
	GL_UNSIGNED_SHORT                                             = 0x1403, // reuse ColorPointerType, decimal value: 5123
	GL_UNSIGNED_INT                                               = 0x1405, // reuse ColorPointerType, decimal value: 5125
    GL_COLOR_INDEX                                                = 0x1900, // decimal value: 6400
    GL_STENCIL_INDEX                                              = 0x1901, // decimal value: 6401
	// GL_DEPTH_COMPONENT                                            = 0x1902, // reuse InternalFormat, decimal value: 6402
	// GL_RED                                                        = 0x1903, // reuse InternalFormat, decimal value: 6403
    GL_GREEN                                                      = 0x1904, // decimal value: 6404
    GL_BLUE                                                       = 0x1905, // decimal value: 6405
    GL_ALPHA                                                      = 0x1906, // decimal value: 6406
	// GL_RGB                                                        = 0x1907, // reuse InternalFormat, decimal value: 6407
	// GL_RGBA                                                       = 0x1908, // reuse InternalFormat, decimal value: 6408
    GL_LUMINANCE                                                  = 0x1909, // decimal value: 6409
    GL_LUMINANCE_ALPHA                                            = 0x190A, // decimal value: 6410
    GL_ABGR_EXT                                                   = 0x8000, // decimal value: 32768
    GL_CMYK_EXT                                                   = 0x800C, // decimal value: 32780
    GL_CMYKA_EXT                                                  = 0x800D, // decimal value: 32781
    GL_BGR                                                        = 0x80E0, // decimal value: 32992
    GL_BGRA                                                       = 0x80E1, // decimal value: 32993
    GL_YCRCB_422_SGIX                                             = 0x81BB, // decimal value: 33211
    GL_YCRCB_444_SGIX                                             = 0x81BC, // decimal value: 33212
 	// GL_RG                                                         = 0x8227, // reuse InternalFormat, decimal value: 33319
    GL_RG_INTEGER                                                 = 0x8228, // decimal value: 33320
 	// GL_DEPTH_STENCIL                                              = 0x84F9, // reuse InternalFormat, decimal value: 34041
    GL_RED_INTEGER                                                = 0x8D94, // decimal value: 36244
    GL_GREEN_INTEGER                                              = 0x8D95, // decimal value: 36245
    GL_BLUE_INTEGER                                               = 0x8D96, // decimal value: 36246
    GL_RGB_INTEGER                                                = 0x8D98, // decimal value: 36248
    GL_RGBA_INTEGER                                               = 0x8D99, // decimal value: 36249
    GL_BGR_INTEGER                                                = 0x8D9A, // decimal value: 36250
    GL_BGRA_INTEGER                                               = 0x8D9B, // decimal value: 36251
};

enum GLPixelType {
 	GL_BYTE                                                       = 0x1400, // reuse ColorPointerType, decimal value: 5120
 	GL_UNSIGNED_BYTE                                              = 0x1401, // reuse ColorPointerType, decimal value: 5121
 	GL_SHORT                                                      = 0x1402, // reuse ColorPointerType, decimal value: 5122
 	// GL_UNSIGNED_SHORT                                             = 0x1403, // reuse ColorPointerType, decimal value: 5123
 	GL_INT                                                        = 0x1404, // reuse ColorPointerType, decimal value: 5124
	// GL_UNSIGNED_INT                                               = 0x1405, // reuse ColorPointerType, decimal value: 5125
	GL_FLOAT                                                      = 0x1406, // reuse ColorPointerType, decimal value: 5126
    GL_BITMAP                                                     = 0x1A00, // decimal value: 6656
    GL_UNSIGNED_BYTE_3_3_2                                        = 0x8032, // decimal value: 32818
    GL_UNSIGNED_BYTE_3_3_2_EXT                                    = 0x8032, // decimal value: 32818
    GL_UNSIGNED_SHORT_4_4_4_4                                     = 0x8033, // decimal value: 32819
    GL_UNSIGNED_SHORT_4_4_4_4_EXT                                 = 0x8033, // decimal value: 32819
    GL_UNSIGNED_SHORT_5_5_5_1                                     = 0x8034, // decimal value: 32820
    GL_UNSIGNED_SHORT_5_5_5_1_EXT                                 = 0x8034, // decimal value: 32820
    GL_UNSIGNED_INT_8_8_8_8                                       = 0x8035, // decimal value: 32821
    GL_UNSIGNED_INT_8_8_8_8_EXT                                   = 0x8035, // decimal value: 32821
    GL_UNSIGNED_INT_10_10_10_2                                    = 0x8036, // decimal value: 32822
    GL_UNSIGNED_INT_10_10_10_2_EXT                                = 0x8036, // decimal value: 32822
};

} // namespace doi
