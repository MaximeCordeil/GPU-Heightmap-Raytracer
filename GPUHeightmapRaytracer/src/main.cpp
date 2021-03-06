/***********************************************************
* A Template for building OpenGL applications using GLUT
*
* Author: Perspective @ cprogramming.com
* Date : Jan, 2005
*
* Description:
* This code initializes an OpenGL ready window
* using GLUT. Some of the most common callbacks
* are registered to empty or minimal functions.
*
* This code is intended to be a quick starting point
* when building GLUT applications.
*
* Source: http://www.cprogramming.com/snippets/source-code/a-code-template-for-opengl-divide-glut
*
***********************************************************/

#define GLM_FORCE_CUDA

#include <iostream>
#include <fstream>
#include <thread>
#include <string>
#include <chrono>

#include <GL/glew.h>
#include <GL/freeglut.h>
#include <glm/glm.hpp>
#include <glm/gtx/rotate_vector.hpp>

#include <liblas/liblas.hpp>

#include <cuda_gl_interop.h>
#include <cuda_runtime.h>

#include "CudaKernel.cuh"
#include "helper_cuda.h"

#include <Windows.h>


//============================
//		GLOBAL VARIABLES
//============================

// Filenames
std::string point_cloud_file = "autzen.las";
std::string color_map_file = "autzen.jpg";

// Camera related
glm::ivec2 texture_resolution(1920, 1080);
glm::vec3
	camera_position(0, 0, 0),
	camera_point_buffer(0, 0, 0),
	camera_forward(glm::normalize(glm::vec3(0, -.9, 1))),
	frame_dimension(16*2, 9*2, 20); //width, height, distance from camera
glm::vec2 boundaries(0, 0);

GLuint textureID;
GLuint bufferID;
bool use_LOD = false;
bool use_color_map = false;

// JPEG image
CudaSpace::Color *h_color_map;
glm::ivec2 color_map_resolution = glm::zero<glm::ivec2>();

// Point buffer to be copied to GPU
float* h_point_buffer;
glm::ivec2 point_buffer_resolution(32, 32);

// CPU-Side point sections
const int point_sections_size = 4;
std::thread* thread_pool[point_sections_size][point_sections_size];
bool * thread_exit[point_sections_size][point_sections_size];
float* point_sections[point_sections_size][point_sections_size];
CudaSpace::Color* color_sections[point_sections_size][point_sections_size];
glm::vec2 point_sections_origins[point_sections_size][point_sections_size];
glm::vec3 cell_size; //Cell size at the finest LOD level
float max_height = 0;
float height_tolerance = 10;
const int LOD_levels = 8;
int stride_x; // Number of elements per Quad-tree root
int LOD_resolutions[LOD_levels];
int LOD_indexes[LOD_levels];

// clock
std::chrono::system_clock sys_clock;
std::chrono::time_point<std::chrono::system_clock> last_frame, current_frame;
std::chrono::duration<float> delta_time;

// movement
float movement_rht = 0;
float movement_fwd = 0;
float movement_up = 0;
float right_movement = 0;
float wasd_movement_distance = 250;
float qe_movement_distance = 500;

// rotation
float rotation_up = 0;
float rotation_right = 0;
float ik_rotation_angle = 1.f;
float jl_rotation_angle = 1.f;

//============================
//		CUDA VARIABLES
//============================

float* d_point_buffer;
CudaSpace::Color *d_color_map;
struct cudaGraphicsResource* cuda_pbo_resource;

//============================
//		LAS FUNCTIONS
//============================

/*
 * Read LAS header before starting the ray tracing and collect necessary information
 * Set the camera position to the center of the point cloud
 * Source: http://www.liblas.org/tutorial/cpp.html
 */
void readLASHeader(std::string filename)
{
	/*Create input stream and associate it with .las file opened to read in binary mode*/
	std::ifstream ifs;
	ifs.open("../Data/" + filename, std::ios::in | std::ios::binary);
	if (!ifs.is_open())
	{
		std::cout << "Error opening " + filename << std::endl;
		exit(1);
	}

	/*Create a ReaderFactory and instantiate a new liblas::Reader using the stream.*/
	liblas::ReaderFactory f;
	liblas::Reader reader = f.CreateWithStream(ifs);

	/*After the reader has been created, you can access members of the Public Header Block*/
	liblas::Header const& header = reader.GetHeader();
	std::cout << "LAS File Loaded." << std::endl;
	std::cout << "Compressed: " << (header.Compressed() == true) << std::endl;
	std::cout << "Points count: " << header.GetPointRecordsCount() << std::endl;
	std::cout << "MinX: " << header.GetMinX() << " MinY: " << header.GetMinY() << " MinZ: " << header.GetMinZ() << std::endl;
	std::cout << "MaxX: " << header.GetMaxX() << " MaxY: " << header.GetMaxY() << " MaxZ: " << header.GetMaxZ() << std::endl;
	std::cout << "ScaleX: " << header.GetScaleX() << " ScaleY: " << header.GetScaleY() << " ScaleZ: " << header.GetScaleZ() << std::endl;
	std::cout << "OffsetX: " << header.GetOffsetX() << " OffsetY: " << header.GetOffsetY() << " OffsetZ: " << header.GetOffsetZ() << std::endl;
	double deltaX, deltaY;
	deltaX = header.GetMaxX() - header.GetMinX();
	deltaY = header.GetMaxY() - header.GetMinY();
	std::cout << "DiffX: " << deltaX << " DiffY: " << deltaY << std::endl;

	/*Calculate area per point to set cell dimension*/
	float value = 2.0f;
	cell_size = glm::vec3(value, value, value);
	boundaries = glm::vec2(deltaX / cell_size.x, deltaY / cell_size.y);

	/*Place the camera on the first point of the cloud*/
	reader.ReadNextPoint();
	liblas::Point const& p = reader.GetPoint();
	camera_position = glm::vec3((p.GetX() - header.GetMinX()) / cell_size.x, (header.GetMaxZ() - header.GetMinZ())/cell_size.z, (p.GetY() - header.GetMinY()) / cell_size.x);

	/*Set max height for visualization*/
	max_height = static_cast<float>(header.GetMaxZ() - header.GetMinZ())/cell_size.z;

	/*Close the file stream*/
	ifs.close();
}

/*
* Load points using libLas Library in LAS format
* Source: http://www.liblas.org/tutorial/cpp.html
*/
void loadLASToSection(std::string filename, glm::vec2 origin, bool *exit_control, float *point_section, CudaSpace::Color * color_section)
{
	/*Create input stream and associate it with .las file opened to read in binary mode*/
	std::ifstream ifs;
	ifs.open("../Data/" + filename, std::ios::in | std::ios::binary);
	if (!ifs.is_open())
	{
		std::cout << "Error opening " + filename << std::endl;
		exit(1);
	}

	/*Create a ReaderFactory and instantiate a new liblas::Reader using the stream.*/
	liblas::ReaderFactory f;
	liblas::Reader reader = f.CreateWithStream(ifs);

	/*After the reader has been created, you can access members of the Public Header Block*/
	liblas::Header const& header = reader.GetHeader();

	/*Iterate through point records and calculate the height contribution to each neighboring grid cell*/
	while (reader.ReadNextPoint())
	{
		liblas::Point const& p = reader.GetPoint();
		int x, y; // X and Y coordinates in the finest LOD
		unsigned int index[LOD_levels];
		float fX, fY, fZ;

		fX = static_cast<float>(p.GetX() - header.GetMinX()) / cell_size.x;
		fY = static_cast<float>(p.GetY() - header.GetMinY()) / cell_size.y;
		fZ = static_cast<float>(p.GetZ() - header.GetMinZ()) / cell_size.z;

		/* Calculate point position for the finest LOD in this section */
		x = static_cast<int>(glm::floor(fX - origin.x));
		y = static_cast<int>(glm::floor(fY - origin.y));

		/* Skip if the point is outside of the section */
		if (x < 0 || x >= LOD_resolutions[0] || y < 0 || y >= LOD_resolutions[0] || p.GetClassification().GetClass() == 7)
			continue;

		/* Calculate LOD offsets in section from the coarsest to the finest */
		for (int i = LOD_levels - 1; i >= 0; i--)
		{
			index[i] = LOD_indexes[i] + x / static_cast<int>(glm::pow(2.f, i)) + y / static_cast<int>(glm::pow(2.f, i)) * LOD_resolutions[i];
		}

		/* Break the loop if the thread must be terminated */
		if (*exit_control)
			break;

		/*Insert color values in the color map*/
		liblas::Color const& c = p.GetColor();
		color_section[x + y * LOD_resolutions[0]] = CudaSpace::Color(c.GetRed(), c.GetGreen(), c.GetBlue());

		/*Insert the highest values from finest to coarsest level of the Quad-tree*/
		for (int i = 0; i < LOD_levels; i++)
		{
			if (*(point_section + index[i]) <= fZ)
				*(point_section + index[i]) = fZ;
			else
				break;
		}
	}

	/*Close the file stream*/
	ifs.close();

	/*Wait until the thread is unloaded to delete the point data*/
	while (!*exit_control) std::this_thread::yield();
	delete[]point_section;	
	delete[]color_section;
	delete exit_control;
}


//============================
//		SECTION AND GRID
//          FUNCTIONS
//============================

/*
 * Allocate a grid section for the out-of-core functionality
 * The quad-tree piramid is allocate contiguously to facilitate the copy of a section
 */
void allocateSection(glm::ivec2 pos, glm::vec2 origin)
{
	point_sections_origins[pos.x][pos.y] = origin;
	point_sections[pos.x][pos.y] = new float[stride_x * point_buffer_resolution.x * point_buffer_resolution.y]();
	color_sections[pos.x][pos.y] = new CudaSpace::Color[LOD_resolutions[0] * LOD_resolutions[0]];
	thread_exit[pos.x][pos.y] = new bool(false);
	thread_pool[pos.x][pos.y] = new std::thread(loadLASToSection, point_cloud_file, origin, thread_exit[pos.x][pos.y], point_sections[pos.x][pos.y], color_sections[pos.x][pos.y]);

	/* Set inner threads' priority higher than outers' */
	if(pos.x >= 1 && pos.x <=2 && pos.y >= 1 && pos.y <= 2)
		SetThreadPriority(thread_pool[pos.x][pos.y]->native_handle(), 0);
	else
		SetThreadPriority(thread_pool[pos.x][pos.y]->native_handle(), -2);
}

/* 
 * Spawn threads at the initialization phase to start loading points 
 * The camera starts at the center of the sections grid (readLASHeader() must be called before this function)
 */
void initializeSections()
{
	for(int i = 0; i < point_sections_size; i++)
	{
		for (int j = 0; j < point_sections_size; j++)
		{
			allocateSection(glm::ivec2(i, j),
				glm::vec2(camera_position.x, camera_position.z) +
				glm::vec2((i - static_cast<float>(point_sections_size) / 2.0f) * glm::pow(2.0f, LOD_levels - 1) * static_cast<float>(point_buffer_resolution.x),
						  (j - static_cast<float>(point_sections_size) / 2.0f) * glm::pow(2.0f, LOD_levels - 1) * static_cast<float>(point_buffer_resolution.y)));
		}
	}

}

/*
 * Helper function for manageSections
 */
void unloadSectionsColumn(int column)
{
	for(int i = 0; i < point_sections_size; i++)
	{
		if (thread_pool[column][i]->joinable())
		{
			*thread_exit[column][i] = true;
			/*Detach to let the thread end on its own after the object has been deleted*/
			thread_pool[column][i]->detach();
			delete thread_pool[column][i];
		}
	}
}


/*
* Helper function for manageSections
*/
void unloadSectionsRow(int row)
{
	for (int i = 0; i < point_sections_size; i++)
	{
		if (thread_pool[i][row]->joinable())
		{
			*thread_exit[i][row] = true;
			/*Detach to let the thread end on its own after the object has been deleted*/
			thread_pool[i][row]->detach();
			delete thread_pool[i][row];
		}
	}
}

/*
* Move sections X cells horizontally
* + is RIGHT
*/
void rearrangeSectionsX(int x)
{
	int i, j;
	if (x >= 0)
	{
		for (i = point_sections_size - 1; i >= x; i--)
			for (j = 0; j < point_sections_size; j++)
			{
				point_sections[i][j] = point_sections[i - x][j];
				color_sections[i][j] = color_sections[i - x][j];
				point_sections_origins[i][j] = point_sections_origins[i - x][j];
				thread_pool[i][j] = thread_pool[i - x][j];
				if((j >= 1 || j <= 2) && ( i >= 1 && i <= 2))
					SetThreadPriority(thread_pool[i][j]->native_handle(), 0);
				else
					SetThreadPriority(thread_pool[i][j]->native_handle(), -2);
			}
	}
	else
	{
		for (i = 0; i < point_sections_size + x; i++)
			for (j = 0; j < point_sections_size; j++)
			{
				point_sections[i][j] = point_sections[i - x][j];
				color_sections[i][j] = color_sections[i - x][j];
				point_sections_origins[i][j] = point_sections_origins[i - x][j];
				thread_pool[i][j] = thread_pool[i - x][j];
				if ((j >= 1 || j <= 2) && (i >= 1 && i <= 2))
					SetThreadPriority(thread_pool[i][j]->native_handle(), 0);
				else
					SetThreadPriority(thread_pool[i][j]->native_handle(), -2);
			}
	}
}

/*
* Move sections Y cells vertically
* + is DOWN
*/
void rearrangeSectionsY(int y)
{
	int i, j;
	if (y >= 0)
	{
		for (i = 0; i < point_sections_size; i++)
			for (j = point_sections_size - 1; j >= y; j--)
			{
				point_sections[i][j] = point_sections[i][j - y];
				color_sections[i][j] = color_sections[i][j - y];
				point_sections_origins[i][j] = point_sections_origins[i][j - y];
				thread_pool[i][j] = thread_pool[i][j - y];
				if ((j >= 1 || j <= 2) && (i >= 1 && i <= 2))
					SetThreadPriority(thread_pool[i][j]->native_handle(), 0);
				else
					SetThreadPriority(thread_pool[i][j]->native_handle(), -2);
			}
	}
	else
	{
		for (i = 0; i < point_sections_size; i++)
			for (j = 0; j < point_sections_size + y; j++)
			{
				point_sections[i][j] = point_sections[i][j - y];
				color_sections[i][j] = color_sections[i][j - y];
				point_sections_origins[i][j] = point_sections_origins[i][j - y];
				thread_pool[i][j] = thread_pool[i][j - y];
				if ((j >= 1 || j <= 2) && (i >= 1 && i <= 2))
					SetThreadPriority(thread_pool[i][j]->native_handle(), 0);
				else
					SetThreadPriority(thread_pool[i][j]->native_handle(), -2);
			}
	}
}

/* 
 * Based on camera position, load and unload point sections
 * If the camera's grid is less than the set distance to a border, rearrange the grid
 */
void manageSections()
{
	/*Allocate left - move sections right*/
	if (camera_position.x < point_sections_origins[1][0].x)
	{
		unloadSectionsColumn(point_sections_size - 1);
		rearrangeSectionsX(1);
		for (int i = 0; i < point_sections_size; i++)
			allocateSection(glm::ivec2(0, i),
				point_sections_origins[1][i] - glm::vec2(1, 0) * static_cast<float>(point_buffer_resolution.x) *  glm::pow(2.0f, LOD_levels - 1));
	}

	/*Allocate right - Move sections left*/
	if (camera_position.x >= point_sections_origins[point_sections_size - 1][point_sections_size - 1].x)
	{
		unloadSectionsColumn(0);
		rearrangeSectionsX(-1);
		for (int i = 0; i < point_sections_size; i++)
			allocateSection(glm::ivec2(point_sections_size - 1, i),
				point_sections_origins[point_sections_size - 2][i] + glm::vec2(1, 0) * static_cast<float>(point_buffer_resolution.x) * glm::pow(2.0f, LOD_levels - 1));
	}

	/*Allocate down - move sections up*/
	if (camera_position.z < point_sections_origins[0][1].y)
	{
		unloadSectionsRow(point_sections_size - 1);
		rearrangeSectionsY(1);
		for (int i = 0; i < point_sections_size; i++)
			allocateSection(glm::ivec2(i, 0),
				point_sections_origins[i][1] - glm::vec2(0, 1) * static_cast<float>(point_buffer_resolution.y) * glm::pow(2.0f, LOD_levels - 1));
	}

	/*Allocate up - move sections down*/
	if (camera_position.z >= point_sections_origins[0][point_sections_size - 1].y)
	{
		unloadSectionsRow(0);
		rearrangeSectionsY(-1);
		for (int i = 0; i < point_sections_size; i++)
			allocateSection(glm::ivec2(i, point_sections_size - 1),
				point_sections_origins[i][point_sections_size - 2] + glm::vec2(0, 1) * static_cast<float>(point_buffer_resolution.y) * glm::pow(2.0f, LOD_levels - 1));
	}
}


/*
* Set up CPU-Side buffer that is going to be transferred over to the GPU
*
* NOTE: it is more efficient to pre-allocate the point
* buffer in the RAM and then pass it to the GPU
* than passing every line at a time
*
*/
void preparePointBuffer()
{
	glm::vec2 bottom_left, top_right, offset;
	int minX, maxX, minY, maxY;

	/*Set the corners of the point buffer*/
	offset =
		glm::vec2(glm::pow(2.0f, LOD_levels - 1) * point_buffer_resolution.x / 2.0f,
		glm::pow(2.0f, LOD_levels - 1) * point_buffer_resolution.y / 2.0f);

	bottom_left = glm::vec2(camera_position.x, camera_position.z) - offset;
	top_right = glm::vec2(camera_position.x, camera_position.z) + offset - glm::vec2(FLT_MIN, FLT_MIN); //subtract an amount in case the camera is at the center of a grid 

	/*Left section index*/
	minX = 0;
	while(bottom_left.x > point_sections_origins[minX][0].x && minX < point_sections_size)
	{
		minX++;
	}
	minX--;

	/*Bottom section index*/
	minY = 0;
	while (bottom_left.y > point_sections_origins[0][minY].y && minY < point_sections_size)
	{
		minY++;
	}
	minY--;

	/*Right section index*/
	maxX = 0;
	while(top_right.x > point_sections_origins[maxX][0].x && maxX < point_sections_size)
	{
		maxX++;
	}
	maxX--;

	/*Top section index*/
	maxY = 0;
	while (top_right.y > point_sections_origins[0][maxY].y && maxY < point_sections_size)
	{
		maxY++;
	}
	maxY--;

	/*Copy the quad-trees into the point buffer, start with lower left corner and proceed row-wise */
	glm::vec2 section_position;
	glm::ivec2 cell_position;
	int row_index, row_offset;

	/*Section position at lower left section*/
	section_position = bottom_left - point_sections_origins[minX][minY];
	cell_position = glm::ivec2(static_cast<int>(glm::floor(section_position.x / glm::pow(2.0f, LOD_levels - 1))), static_cast<int>(glm::floor(section_position.y / glm::pow(2.0f, LOD_levels - 1))));

	/*Set the camera position in the correct position inside the buffer based on the distance from cell origin to point_buffer origin*/
	camera_point_buffer = 
		glm::vec3((section_position.x - cell_position.x * glm::pow(2.0f, LOD_levels - 1)) + (point_buffer_resolution.x - 1) * glm::pow(2.0f, LOD_levels - 2),
				  camera_position.y,
				  (section_position.y - cell_position.y * glm::pow(2.0f, LOD_levels - 1)) + (point_buffer_resolution.y - 1) * glm::pow(2.0f, LOD_levels - 2));

	for (int i = LOD_levels - 1; i >= 0; i--)
	{
		/*Copy the data from the lower left section*/
		row_offset = 0;
		for (row_index = cell_position.y; row_index < LOD_resolutions[i]; row_index++)
		{
			memcpy(h_point_buffer + LOD_indexes[i] + row_offset * LOD_resolutions[i],
				point_sections[minX][minY] + LOD_indexes[i] + cell_position.x + row_index * LOD_resolutions[i],
				sizeof(float) * (LOD_resolutions[i] - cell_position.x));

			row_offset++;
		}

		/*Copy the data from the bottom right section*/
		row_offset = 0;
		row_index = cell_position.x == 0 ? LOD_resolutions[i] : cell_position.y;
		for (row_index; row_index < LOD_resolutions[i]; row_index++)
		{
			memcpy(h_point_buffer + LOD_indexes[i] + (LOD_resolutions[i] - cell_position.x) + row_offset * LOD_resolutions[i],
				point_sections[maxX][minY] + LOD_indexes[i] + row_index * LOD_resolutions[i],
				sizeof(float) * cell_position.x);

			row_offset++;
		}

		/*Copy the data from top left section */
		row_offset = 0;
		row_index = cell_position.y == 0 ? cell_position.y : 0;
		for (row_index; row_index < cell_position.y; row_index++)
		{
			memcpy(h_point_buffer + LOD_indexes[i] + (row_index + LOD_resolutions[i] - cell_position.y) * LOD_resolutions[i],
				point_sections[minX][maxY] + LOD_indexes[i] + cell_position.x + row_offset * LOD_resolutions[i],
				sizeof(float) * (LOD_resolutions[i] - cell_position.x));

			row_offset++;
		}

		/*Copy the data from top right section*/
		row_offset = 0;
		row_index = cell_position.y == 0 || cell_position.x == 0 ? cell_position.y : 0;
		for (row_index; row_index < cell_position.y; row_index++)
		{
			memcpy(h_point_buffer + LOD_indexes[i] + (LOD_resolutions[i] - cell_position.x) + (row_index + LOD_resolutions[i] - cell_position.y) * LOD_resolutions[i],
				point_sections[maxX][maxY] + LOD_indexes[i] + row_offset * LOD_resolutions[i],
				sizeof(float) * cell_position.x);

			row_offset++;
		}
		if(i > 0)
			cell_position *= 2;
	}

	/*Copy color data */
	/*Copy the data from the lower left section*/
	row_offset = 0;
	for (row_index = cell_position.y; row_index < LOD_resolutions[0]; row_index++)
	{
		memcpy(h_color_map + row_offset * LOD_resolutions[0],
			color_sections[minX][minY] +  cell_position.x + row_index * LOD_resolutions[0],
			sizeof(CudaSpace::Color) * (LOD_resolutions[0] - cell_position.x));

		row_offset++;
	}

	/*Copy the data from the bottom right section*/
	row_offset = 0;
	row_index = cell_position.x == 0 ? LOD_resolutions[0] : cell_position.y;
	for (row_index; row_index < LOD_resolutions[0]; row_index++)
	{
		memcpy(h_color_map + (LOD_resolutions[0] - cell_position.x) + row_offset * LOD_resolutions[0],
			color_sections[maxX][minY] + row_index * LOD_resolutions[0],
			sizeof(CudaSpace::Color) * cell_position.x);

		row_offset++;
	}

	/*Copy the data from top left section */
	row_offset = 0;
	row_index = cell_position.y == 0 ? cell_position.y : 0;
	for (row_index; row_index < cell_position.y; row_index++)
	{
		memcpy(h_color_map +  + (row_index + LOD_resolutions[0] - cell_position.y) * LOD_resolutions[0],
			color_sections[minX][maxY] + cell_position.x + row_offset * LOD_resolutions[0],
			sizeof(CudaSpace::Color) * (LOD_resolutions[0] - cell_position.x));

		row_offset++;
	}

	/*Copy the data from top right section*/
	row_offset = 0;
	row_index = cell_position.y == 0 || cell_position.x == 0 ? cell_position.y : 0;
	for (row_index; row_index < cell_position.y; row_index++)
	{
		memcpy(h_color_map + (LOD_resolutions[0] - cell_position.x) + (row_index + LOD_resolutions[0] - cell_position.y) * LOD_resolutions[0],
			color_sections[maxX][maxY] + row_offset * LOD_resolutions[0],
			sizeof(CudaSpace::Color) * cell_position.x);

		row_offset++;
	}
}

void copyPointBuffer()
{
	/*Send point buffer to the gpu*/
	checkCudaErrors(cudaMemcpy(d_point_buffer, h_point_buffer, sizeof(float) * point_buffer_resolution.x * stride_x * point_buffer_resolution.y, cudaMemcpyHostToDevice));
	checkCudaErrors(cudaMemcpy(d_color_map, h_color_map, sizeof(CudaSpace::Color) * LOD_resolutions[0] * LOD_resolutions[0], cudaMemcpyHostToDevice));
}
/*
 * This method sets up a texture object and its respective buffers to share with CUDA device
 *
 * GL_TEXTURE_RECTANGLE is used to avoid generating mip maps and wasting resources
 * Sources: 
 * https://www.khronos.org/opengl/wiki/Rectangle_Texture
 * http://www.songho.ca/opengl/gl_pbo.html
 *
 */
void setupTexture()
{
	// Generate a buffer ID
	glGenBuffers(1, &bufferID);
	// Make this the current UNPACK buffer (OpenGL is state-based)
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, bufferID);
	// Allocate data for the buffer. 4-channel 8-bit image
	glBufferData(GL_PIXEL_UNPACK_BUFFER, texture_resolution.x * texture_resolution.y * 3, NULL, GL_DYNAMIC_COPY);
	// Registers the buffer object specified by buffer for access by CUDA.A handle to the registered object is returned as resource.
	// Source: http://docs.nvidia.com/cuda/cuda-runtime-api/group__CUDART__OPENGL.html#group__CUDART__OPENGL_1g0fd33bea77ca7b1e69d1619caf44214b
	checkCudaErrors(cudaGraphicsGLRegisterBuffer(&cuda_pbo_resource, bufferID, cudaGraphicsRegisterFlagsNone));

	// Enable Texturing
	glEnable(GL_TEXTURE_RECTANGLE);
	// Generate a texture ID
	glGenTextures(1, &textureID);
	// Make this the current texture (remember that GL is state-based)
	glBindTexture(GL_TEXTURE_RECTANGLE, textureID);
	// Allocate the texture memory. The last parameter is NULL since we only want to allocate memory, not initialize it
	// https://www.khronos.org/opengl/wiki/GLAPI/glTexBuffer
	glTexImage2D(GL_TEXTURE_RECTANGLE, 0, GL_RGB8, texture_resolution.x, texture_resolution.y, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);

	// Must set the filter mode to avoid any altering in the resulting image and reduce performance cost
	glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_S, GL_CLAMP);
	glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_T, GL_CLAMP);

	// Unbind texture and buffer
	glBindTexture(GL_TEXTURE_RECTANGLE, 0);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
}


//============================
//		OPENGL FUNCTIONS
//============================
/*
 * Update the texture with raytracing data from the GPU
 */
void updateTexture()
{
	//Synchronize OpenGL and CPU calls before locking and working on the buffer object
	checkCudaErrors(cudaGraphicsMapResources(1, &cuda_pbo_resource, 0));

	//Get a pointer to the memory position in the CUDA device (GPU)
	unsigned char* devPtr;
	size_t size;
	checkCudaErrors(cudaGraphicsResourceGetMappedPointer(reinterpret_cast<void **>(&devPtr), &size, cuda_pbo_resource));

	//Call the wrapper function invoking the CUDA Kernel
	CudaSpace::rayTrace(texture_resolution, frame_dimension, camera_forward, camera_point_buffer, devPtr, use_color_map, max_height);

	//Synchronize CUDA calls and release the buffer for OpenGL and CPU use;
	checkCudaErrors(cudaGraphicsUnmapResources(1, &cuda_pbo_resource, 0));
}

/*
 * Copy pixel data to a texture and display it on screen
 */
void renderTexture()
{
	int width = glutGet(GLUT_WINDOW_WIDTH),
		height = glutGet(GLUT_WINDOW_HEIGHT);

	// Copy texture data from buffer;
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, bufferID);
	glBindTexture(GL_TEXTURE_RECTANGLE, textureID);
	glTexSubImage2D(GL_TEXTURE_RECTANGLE, 0, 0, 0, texture_resolution.x, texture_resolution.y, GL_RGB, GL_UNSIGNED_BYTE, NULL);


	// Adjust coordinate system to screen position
	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	glLoadIdentity();
	glOrtho(0.0, width, 0.0, height, -1.0, 1.0);
	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();


	// Draw a quad and apply texture
	glEnable(GL_TEXTURE_RECTANGLE);
	{
		glBegin(GL_QUADS);

		glTexCoord2i(0, 0); 
		glVertex2i(0, 0);

		glTexCoord2i(texture_resolution.x, 0);	
		glVertex2i(width, 0);

		glTexCoord2i(texture_resolution.x, texture_resolution.y);
		glVertex2i(width, height);

		glTexCoord2i(0, texture_resolution.y);
		glVertex2i(0, height);
		glEnd();
	}
	glDisable(GL_TEXTURE_RECTANGLE);

	// Unbind buffer and texture
	glBindTexture(GL_TEXTURE_RECTANGLE, 0);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

	glPopMatrix();
	glMatrixMode(GL_PROJECTION);
	glPopMatrix();
	glMatrixMode(GL_MODELVIEW);
}


//============================
//		CAMERA FUNCTIONS
//============================

/*
 *Handle the camera movement
 */
void moveCamera()
{
	float factor = delta_time.count() > 1 ? 1 : delta_time.count();
	camera_position += factor * (glm::vec3(0, movement_up, 0) + glm::normalize(glm::vec3(camera_forward.x, 0, camera_forward.z)) * movement_fwd + glm::normalize(glm::cross(camera_forward, glm::vec3(0, 1, 0))) * movement_rht);
	
	if (camera_position.x < 0)
		camera_position.x = 0;
	if (camera_position.x >= boundaries.x)
		camera_position.x = boundaries.x - 0.00001f;

	if (camera_position.z < 0)
		camera_position.z = 0;
	if (camera_position.z >= boundaries.y)
		camera_position.z = boundaries.y - 0.00001f;

	if (camera_position.y < 0)
		camera_position.y = 0;
	if (camera_position.y >= max_height * 4)
		camera_position.y = max_height * 4;
}
/*
 * Handle camera rotation
 */
void rotateCamera()
{

	camera_forward = glm::rotate(camera_forward, rotation_up * delta_time.count(), glm::vec3(0, 1, 0));
	camera_forward = glm::rotate(camera_forward, rotation_right * delta_time.count(), glm::normalize(glm::cross(camera_forward, glm::vec3(0, 1, 0))));
}


//============================
//		GLUT FUNCTIONS
//============================
/*Displays FPS from http://stackoverflow.com/questions/20866508/using-glut-to-simply-print-text*/
void drawFPS()
{
	int width = glutGet(GLUT_WINDOW_WIDTH),
		height = glutGet(GLUT_WINDOW_HEIGHT);

	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	glLoadIdentity();
	gluOrtho2D(0, width, 0, height);

	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
	glLoadIdentity();
	glRasterPos2i(10, 10);

	current_frame = sys_clock.now();
	delta_time = current_frame - last_frame;
	last_frame = current_frame;
	std::string text = "FPS " + std::to_string(1 / delta_time.count()) + "| Camera Position: " + std::to_string(camera_position.x) + " " + std::to_string(camera_position.y) + " " + std::to_string(camera_position.z);
	
	for (int i = 0; i < text.length(); ++i) {
		glutBitmapCharacter(GLUT_BITMAP_TIMES_ROMAN_10, text[i]);
	}

	glPopMatrix();

	glMatrixMode(GL_PROJECTION);
	glPopMatrix();
	glMatrixMode(GL_MODELVIEW);
}

/* process menu option 'op' */
void menu(int op)
{
	switch (op)
	{
	case 'Q':
	case 'q':
		exit(0);
	default:;
	}
}

/* executed when a regular key is pressed */
void keyboardDown(unsigned char key, int x, int y)
{
	switch (key)
	{
	case 'p':
	case '27':
		exit(0);
	case '+':
		max_height += 100;
		break;
	case'-':
		max_height -= 100;
		break;

	/* Movement */
	case 'e':
		movement_up = qe_movement_distance;
		break;
	case 'q':
		movement_up = -qe_movement_distance;
		break;
	case'a':
		movement_rht = wasd_movement_distance;
		break;
	case'd':
		movement_rht = -wasd_movement_distance;
		break;
	case 'w':
		movement_fwd = wasd_movement_distance;
		break;
	case 's':
		movement_fwd = -wasd_movement_distance;
		break;

	/* Rotation */
	case 'i':
		rotation_right = ik_rotation_angle;
		break;
	case 'k':
		rotation_right = -ik_rotation_angle;
		break;
	case 'j':
		rotation_up = -jl_rotation_angle;
		break;
	case 'l':
		rotation_up = jl_rotation_angle;
		break;

	/* Visualization parameters */
	case 'r':
		if (h_color_map != NULL)
			use_color_map = !use_color_map;
		break;
	case 't':
		use_LOD = !use_LOD;
	default:;
	}
}

void keyboardUp(unsigned char key, int x, int y)
{
	switch (key)
	{
	case 'p':
	case 27:
		glutLeaveFullScreen();
		glutLeaveMainLoop();
	case 'e':
	case 'q':
		movement_up = 0;
		break;
	case 'a':
	case 'd':
		movement_rht = 0;
		break;
	case 'w':
	case 's':
		movement_fwd = 0;
		break;
	case 'i':
	case 'k':
		rotation_right = 0;
		break;
	case 'j':
	case 'l':
		rotation_up = 0;
	default:;
	}
}

/* reshaped window */
void reshape(int width, int height)
{
	GLfloat fieldOfView = 90.0f;
	glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));

	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	gluPerspective(fieldOfView, static_cast<GLfloat>(width) / static_cast<GLfloat>(height), 0.1, 500.0);

	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
}

/* executed when button 'button' is put into state 'state' at screen position ('x', 'y') */
void mouseClick(int button, int state, int x, int y)
{
}

/* executed when the mouse moves to position ('x', 'y') */
void mouseMotion(int x, int y)
{
}

/* render the scene */
void draw()
{
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	moveCamera();
	rotateCamera();
	manageSections();

	/* render the scene here */
	preparePointBuffer();
	copyPointBuffer();
	updateTexture();
	renderTexture();
	drawFPS();

	glFlush();
	glutSwapBuffers();
}

/* executed when program is idle */
void idle()
{
	draw();
}

/* initialize OpenGL settings */
void initGL(int width, int height)
{
	reshape(width, height);

	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClearDepth(1.0f);

	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LEQUAL);
}


//============================
//        RESOURCES
//============================
/* Allocate resources */
void initialize()
{
	glewInit();

	LOD_resolutions[LOD_levels - 1] = point_buffer_resolution.x;
	LOD_indexes[LOD_levels - 1] = 0;
	stride_x = static_cast<int>(glm::pow(4.f, LOD_levels - 1));
	for (auto i = LOD_levels - 2; i >= 0; i--)
	{
		LOD_indexes[i] = LOD_indexes[i + 1] + LOD_resolutions[i + 1] * LOD_resolutions[i + 1];
		LOD_resolutions[i] = LOD_resolutions[i + 1] * 2;
		stride_x += static_cast<int>(glm::pow(4.f, i));
	}

	readLASHeader(point_cloud_file);
	initializeSections();

	checkCudaErrors(cudaGLSetGLDevice(gpuGetMaxGflopsDeviceId()));
	setupTexture();
	h_point_buffer = new float[point_buffer_resolution.x * point_buffer_resolution.y * stride_x];
	h_color_map = new CudaSpace::Color[LOD_resolutions[0] * LOD_resolutions[0]];
	checkCudaErrors(cudaMalloc(&d_point_buffer, sizeof(float) * point_buffer_resolution.x * point_buffer_resolution.y * stride_x));
	checkCudaErrors(cudaMalloc(&d_color_map, sizeof(CudaSpace::Color) * LOD_resolutions[0] * LOD_resolutions[0]));
	CudaSpace::initializeDeviceVariables(point_buffer_resolution, texture_resolution, d_point_buffer, d_color_map, LOD_levels, stride_x, max_height);

}

/* Free Resources */
void freeResourcers()
{
	checkCudaErrors(cudaDeviceSynchronize());
	checkCudaErrors(cudaFree(d_point_buffer));
	checkCudaErrors(cudaFree(d_color_map));
	CudaSpace::freeDeviceVariables();
	delete[](h_color_map);
	delete[](h_point_buffer);
	for each(std::thread* ptr in thread_pool)
		delete ptr;
	for each(float* ptr in point_sections)
		delete[] ptr;
	for each(CudaSpace::Color* ptr in color_sections)
		delete[] ptr;
	for each(bool* ptr in thread_exit)
		delete[] ptr;
	
}


//============================
//			MAIN
//============================
/* initialize GLUT settings, register callbacks, enter main loop */
int main(int argc, char** argv)
{
	/*
	 *Set main thread priority higher to avoid not being executed for a long time
	 *Source: https://msdn.microsoft.com/en-us/library/windows/desktop/ms685100(v=vs.85).aspx
	 */
	SetThreadPriority(GetCurrentThread(), 2);
	glutInit(&argc, argv);

	glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGB | GLUT_DEPTH);
	glutInitWindowSize(1024, 768);
	glutInitWindowPosition(100, 100);
	glutCreateWindow("GPU Heightmap Raytracer 2k17");

	// register glut call backs
	glutKeyboardFunc(keyboardDown);
	glutKeyboardUpFunc(keyboardUp);
	glutMouseFunc(mouseClick);
	glutMotionFunc(mouseMotion);
	glutReshapeFunc(reshape);
	glutDisplayFunc(draw);
	glutIdleFunc(idle);
	glutIgnoreKeyRepeat(true); // ignore keys held down

	// create a sub menu 
	int subMenu = glutCreateMenu(menu);
	glutAddMenuEntry("Do nothing", 0);
	glutAddMenuEntry("Really Quit", 'q');

	// create main "right click" menu
	glutCreateMenu(menu);
	glutAddSubMenu("Sub Menu", subMenu);
	glutAddMenuEntry("Quit", 'q');
	glutAttachMenu(GLUT_RIGHT_BUTTON);


	initialize();
	atexit(freeResourcers);
	glutFullScreen();
	initGL(1024, 768);

	glutMainLoop();

	return 0;
}
