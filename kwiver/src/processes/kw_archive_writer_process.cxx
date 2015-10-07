/*ckwg +5
 * Copyright 2014 by Kitware, Inc. All Rights Reserved. Please refer to
 * KITWARE_LICENSE.TXT for licensing information, or contact General Counsel,
 * Kitware, Inc., 28 Corporate Drive, Clifton Park, NY 12065.
 */

#include "kw_archive_writer_process.h"

#include <types/kwiver.h>

//#include <maptk/modules.h>
#include <maptk/image_container.h>
#include <maptk/image.h>
#include <maptk/homography.h>
#include <maptk/homography_f2f.h>

#include <sprokit/pipeline/process_exception.h>

#include <fstream>
#include <vector>
#include <stdint.h>

#include <vul/vul_file.h>
#include <vsl/vsl_binary_io.h>
#include <vsl/vsl_vector_io.h>

#include <vil/vil_image_view.h>
#include <vil/vil_pixel_format.h>
#include <vil/vil_stream_core.h>
#include <vil/file_formats/vil_jpeg.h>
#include <vil/io/vil_io_image_view.h>

#include <vnl/io/vnl_io_matrix_fixed.h>
#include <vnl/io/vnl_io_vector_fixed.h>

#include <vnl/vnl_double_2.h>

// instantiate vsl vector routine
#include <vsl/vsl_vector_io.txx>
VSL_VECTOR_IO_INSTANTIATE( char );


namespace kwiver
{

  // -- config items --
  create_config_trait( output_directory, std::string, ".", "Output directory where KWA will be written" );
  create_config_trait( base_filename, std::string, "", "Base file name (no extension) for KWA component files" );
  create_config_trait( separate_meta, bool, "true", "Whether to write separate .meta file" );
  create_config_trait( mission_id, std::string, "", "Mission ID to store in archive" );
  create_config_trait( stream_id, std::string, "", "Stream ID to store in archive" );
  create_config_trait( compress_image, bool, "true", "Whether to compress image data stored in archive" );

//----------------------------------------------------------------
// Private implementation class
class kw_archive_writer_process::priv
{
public:
  priv();
  ~priv();

  void write_frame_data(vsl_b_ostream& stream,
                        bool write_image,
                        kwiver::timestamp const& time,
                        kwiver::geo_polygon_t const& corners,
                        maptk::image const& img,
                        maptk::f2f_homography const& homog,
                        kwiver::gsd_t gsd);

  static sprokit::process::port_t const port_timestamp;
  static sprokit::process::port_t const port_image;
  static sprokit::process::port_t const port_src_to_ref_homography;
  static sprokit::process::port_t const port_corner_points;
  static sprokit::process::port_t const port_gsd;


  // Configuration values
  std::string m_output_directory;
  std::string m_base_filename;
  bool m_separate_meta;
  std::string m_mission_id;
  std::string m_stream_id;
  bool m_compress_image;

  // local storage
  std::ofstream* m_index_stream;
  std::ofstream* m_meta_stream;
  vsl_b_ostream* m_meta_bstream;
  std::ofstream* m_data_stream;
  vsl_b_ostream* m_data_bstream;

  int m_data_version;
  std::vector < char > m_image_write_cache;

}; // end priv class

#define priv_t kw_archive_writer_process::priv

// ================================================================

kw_archive_writer_process
::kw_archive_writer_process( sprokit::config_t const& config )
  : process(config),
    d( new kw_archive_writer_process::priv )
{
  make_ports();
  make_config();
}


kw_archive_writer_process
::~kw_archive_writer_process( )
{
}


// ----------------------------------------------------------------
void
kw_archive_writer_process
::_configure()
{
  // Examine the configuration
  d->m_output_directory = config_value_using_trait( output_directory );
  d->m_base_filename    = config_value_using_trait( base_filename );
  d->m_separate_meta    = config_value_using_trait( separate_meta );
  d->m_mission_id       = config_value_using_trait( mission_id );
  d->m_stream_id        = config_value_using_trait( mission_id );
  d->m_compress_image   = config_value_using_trait( compress_image );

  sprokit::process::_configure();
}


// ----------------------------------------------------------------
// Post connection initialization
void
kw_archive_writer_process
::_init()
{
  std::string path = d->m_output_directory + "/" + d->m_base_filename;

  // Make sure directory exists
  vul_file::make_directory_path( d->m_output_directory );

  std::string index_filename = path + ".index";
  std::string meta_filename  = path + ".meta";
  std::string data_filename  = path + ".data";

  d->m_index_stream = new std::ofstream( index_filename.c_str(),
                                std::ios::out | std::ios::trunc );
  if ( ! *d->m_index_stream )
  {
    std::string const reason = "Failed to open " + index_filename + " for writing";
    throw sprokit::invalid_configuration_exception( name(), reason );
  }

  if ( d->m_separate_meta )
  {
    // open metadata stream
    d->m_meta_stream = new std::ofstream( meta_filename.c_str(),
             std::ios::out | std::ios::trunc | std::ios::binary );

    if ( ! *d->m_meta_stream )
    {
      std::string const reason = "Failed to open " + meta_filename + " for writing";
      throw sprokit::invalid_configuration_exception( name(), reason );
    }

    d->m_meta_bstream = new vsl_b_ostream( d->m_meta_stream );
  }

  d->m_data_stream = new std::ofstream( data_filename.c_str(),
             std::ios::out | std::ios::trunc | std::ios::binary );
  d->m_data_bstream = new vsl_b_ostream( d->m_data_stream );
  if ( ! *d->m_data_stream )
  {
    std::string const reason = "Failed to open " + data_filename + " for writing";
    throw sprokit::invalid_configuration_exception( name(), reason );
  }

  // Write file headers
  *d->m_index_stream
    << "4\n" // Version number
    << vul_file::basename( data_filename ) << "\n";

  if ( d->m_data_bstream != NULL )
  {
    *d->m_index_stream << vul_file::basename( meta_filename ) << "\n";
  }
  else
  {
    *d->m_index_stream << "\n";
  }

  *d->m_index_stream
    << d->m_mission_id << "\n"
    << d->m_stream_id << "\n";

  // version depends on compression option
  if ( d->m_compress_image )
  {
    d->m_data_version = 3;
  }
  else
  {
    d->m_data_version = 2;
  }

  vsl_b_write( *d->m_data_bstream, d->m_data_version ); // version number

  if ( d->m_meta_bstream )
  {
    vsl_b_write( *d->m_meta_bstream, static_cast< int > ( 2 ) ); // version number
  }

  if ( ! *d->m_index_stream || ! *d->m_data_stream ||
       ( d->m_separate_meta && ! *d->m_meta_stream ) )
  {
    static std::string const reason = "Failed while writing file headers";
    throw sprokit::invalid_configuration_exception( name(), reason );
  }

  process::_init();
} // kw_archive_writer_process::_init


// ----------------------------------------------------------------
void
kw_archive_writer_process
::_step()
{
  // timestamp
  kwiver::timestamp frame_time = grab_input_using_trait( timestamp );

  // image
  //+ maptk::image_container_sptr img = grab_input_as< maptk::image_container_sptr > ( priv::port_image );
  maptk::image_container_sptr img = grab_from_port_using_trait( image );
  maptk::image image = img->get_image();

  // homography
  //+ maptk::f2f_homography homog = grab_input_as< maptk::f2f_homography > ( priv::port_src_to_ref_homography );
  maptk::f2f_homography homog = grab_from_port_using_trait( src_to_ref_homography );

  // corners
  kwiver::geo_polygon_t corners = grab_input_using_trait( corner_points );

  // gsd
  kwiver::gsd_t gsd = grab_input_using_trait( gsd );

  std::cerr << "DEBUG - (KWA_WRITER) processing frame " << frame_time
            << std::endl;

  *d->m_index_stream
    << static_cast< vxl_int_64 > ( frame_time.get_time() * 1e6 ) << " " // in micro-seconds
    << static_cast< int64_t > ( d->m_data_stream->tellp() )
    << std::endl;

  d->write_frame_data( *d->m_data_bstream,
                       /*write image=*/ true,
                       frame_time, corners, image, homog, gsd );
  if ( ! d->m_data_stream )
  {
    // throw ( ); //+ need general runtime exception
    // LOG_DEBUG("Failed while writing to .data stream");
  }

  if ( d->m_meta_bstream )
  {
    d->write_frame_data( *d->m_meta_bstream,
                         /*write48 image=*/ false,
                         frame_time, corners, image, homog, gsd );
    if ( ! d->m_meta_stream )
    {
      // throw ( );
      // LOG_DEBUG("Failed while writing to .meta stream");
    }
  }

  sprokit::process::_step();
} // kw_archive_writer_process::_step




// ----------------------------------------------------------------
void
kw_archive_writer_process
::make_ports()
{
  // Set up for required ports
  sprokit::process::port_flags_t required;
  required.insert( flag_required );

  sprokit::process::port_flags_t opt_static;
  opt_static.insert( flag_input_static );

  // declare input ports
  declare_input_port_using_trait( timestamp, required );
  declare_input_port_using_trait( image, required );
  declare_input_port_using_trait( src_to_ref_homography, required );
  declare_input_port_using_trait( corner_points, opt_static );
  declare_input_port_using_trait( gsd, opt_static );
}


// ----------------------------------------------------------------
void
kw_archive_writer_process
::make_config()
{
  declare_config_using_trait( output_directory );
  declare_config_using_trait( base_filename );
  declare_config_using_trait( separate_meta );
  declare_config_using_trait( mission_id );
  declare_config_using_trait( stream_id );
  declare_config_using_trait( compress_image );
}


// ----------------------------------------------------------------
void
priv_t
::write_frame_data(vsl_b_ostream& stream,
                   bool write_image,
                   kwiver::timestamp const& time,
                   kwiver::geo_polygon_t const& corner_pts,
                   maptk::image const& img,
                   maptk::f2f_homography const& s2r_homog,
                   double gsd)
{
  vxl_int_64 u_seconds = static_cast< vxl_int_64 > ( time.get_time() * 1e6 );
  vxl_int_64 frame_num = static_cast< vxl_int_64 > ( time.get_frame() );
  vxl_int_64 ref_frame_num = static_cast< vxl_int_64 > ( s2r_homog.to_id() );

  // convert image in place
  vil_image_view < vxl_byte > image( img.first_pixel(),
                                     img.width(), // n_i
                                     img.height(), // n_j
                                     img.depth(), // n_planes
                                     img.w_step(), // i_step
                                     img.h_step(), // j_step
                                     img.d_step() // plane_step
    );

  // convert homography
  Eigen::Matrix< double, 3, 3 > matrix= s2r_homog.homography()->matrix();                                                                               
  vnl_matrix_fixed< double, 3, 3 > homog;

  // Copy matrix into vnl format
  for ( int x = 0; x < 3; ++x )
  {
    for ( int y = 0; y < 3; ++y )
    {
      homog( x, y ) = matrix( x, y );
    }
  }

  std::vector< vnl_vector_fixed< double, 2 > > corners; // (x,y)
  corners.push_back( vnl_double_2( corner_pts[0].get_longitude(), corner_pts[0].get_latitude() ) ); // ul
  corners.push_back( vnl_double_2( corner_pts[1].get_longitude(), corner_pts[1].get_latitude() ) ); // ur
  corners.push_back( vnl_double_2( corner_pts[2].get_longitude(), corner_pts[2].get_latitude() ) ); // lr
  corners.push_back( vnl_double_2( corner_pts[3].get_longitude(), corner_pts[3].get_latitude() ) ); // ll

  stream.clear_serialisation_records();
  vsl_b_write( stream, u_seconds );

  if ( write_image )
  {
    if ( this->m_data_version == 3 )
    {
      vsl_b_write( stream, 'J' ); // J=jpeg
      vil_stream* mem_stream = new vil_stream_core();
      mem_stream->ref();
      vil_jpeg_file_format fmt;
      vil_image_resource_sptr img_res =
        fmt.make_output_image( mem_stream,
                               image.ni(), image.nj(), image.nplanes(),
                               VIL_PIXEL_FORMAT_BYTE );
      img_res->put_view( image );
      this->m_image_write_cache.resize( mem_stream->file_size() );
      mem_stream->seek( 0 );
      // LOG_DEBUG( "Compressed image is " << mem_stream->file_size() << " bytes" );
      mem_stream->read( &this->m_image_write_cache[0], mem_stream->file_size() );
      vsl_b_write( stream, this->m_image_write_cache );
      mem_stream->unref(); // allow for automatic delete
    }
    else if ( this->m_data_version == 2 )
    {
      vsl_b_write( stream, image );
    }
    else
    {
      // throw (); unexpected version number
    }
  }

  vsl_b_write( stream, homog );
  vsl_b_write( stream, corners );
  vsl_b_write( stream, gsd );
  vsl_b_write( stream, frame_num );
  vsl_b_write( stream, ref_frame_num );
  vsl_b_write( stream, static_cast< vxl_int_64 > ( image.ni() ) );
  vsl_b_write( stream, static_cast< vxl_int_64 > ( image.nj() ) );

}

// ================================================================
kw_archive_writer_process::priv
::priv()
  : m_index_stream(0),
    m_meta_stream(0),
    m_meta_bstream(0),
    m_data_stream(0),
    m_data_bstream(0)
{
}


kw_archive_writer_process::priv
::~priv()
{
  // Must set pointers to zero to prevent multiple calls from doing
  // bad things.
  delete m_index_stream;
  m_index_stream = 0;

  delete m_meta_bstream;
  m_meta_bstream = 0;

  delete m_meta_stream;
  m_meta_stream = 0;

  delete m_data_bstream;
  m_data_bstream = 0;

  delete m_data_stream;
  m_data_stream = 0;
}


} // end namespace kwiver
