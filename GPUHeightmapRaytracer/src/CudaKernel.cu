﻿#include "CudaKernel.cuh"
#include <helper_cuda.h>
#include <iostream>
#include <math_functions.hpp>



namespace CudaSpace
{
	
	__device__ int grid_resolution;
	__device__ float minimun_ray_inclination = 0.01f;
	__device__ float cell_size[] = {1,2,4};
	__device__ float *point_buffer;
	__device__ float maxHeight, minHeight;
	__device__ glm::vec3 *frame_dimension;
	__device__ glm::vec3 *camera_forward, *camera_right;
	__device__ glm::vec3 *grid_camera_position;
	__device__ glm::ivec2 *texture_resolution;

	/*
	 * GLM::SIGN produces compile error in CUDA
	 */
	__device__ int sign(float input)
	{
		return input >= 0 ? 1 : -1;
	}

	/*
	* Set up parameters for tracing a single ray
	* 
	* Ray always starts in the grid
	*/
	__device__ void setUpParameters(float &tMax, float &tDelta, char &step, int &pos, float ray_position, float ray_direction, int LOD)
	{
		/*Set starting voxel*/
		pos = static_cast<int>(glm::floor(ray_position));

		/*Set whether position should increment or decrement based on ray direction*/
		step = sign(ray_direction);

		/*Calculate the first intersection in the grid*/
		if (ray_direction == 0)
		{
			tMax = FLT_MAX;
			tDelta = 0;
		}
		else
		{
			tMax = ((glm::floor(ray_position / cell_size[LOD]) + (ray_direction < 0 ? 0 : 1)) * cell_size[LOD] - ray_position) / ray_direction;
		}

		/*Calculate the distance between each axis intersection with the grid*/
		tDelta = (ray_direction < 0 ? -1 : 1) * cell_size[LOD] / ray_direction;
	}


	/*
	 *	Cast a ray through the grid {Amanatides, 1987 #22}
	 *	ray_direction MUST be normalized
	 */
	__device__ glm::uvec3 castRay(glm::vec3& ray_position, glm::vec3& ray_direction, int threadId)
	{
		float tMaxX, tMaxZ, tDeltaX, tDeltaZ;
		int posX, posZ;
		char stepX, stepZ;

		int index = 0;
		float tMax = 0;
		glm::vec3 posVector;

		setUpParameters(tMaxX, tDeltaX, stepX, posX, ray_position.x, ray_direction.x, 0);
		setUpParameters(tMaxZ, tDeltaZ, stepZ, posZ, ray_position.z, ray_direction.z, 0);

		/*Check if ray starts outside of the grid*/
		if (posX >= grid_resolution || posX < 0 ||
			posZ >= grid_resolution || posZ < 0)
			return glm::zero<glm::uvec3>();

		/*Check if ray starts under the heightmap*/
		index = posX * grid_resolution + posZ;
		if (point_buffer[index] >= ray_position.y)
			return  glm::zero<glm::uvec3>();


		/*Loop the DDA algorithm*/
		while(true)
		{

			/*Advance ray through the grid*/
			if(tMaxX < tMaxZ)
			{
				posVector = ray_position + ray_direction * tMax;
				tMaxX += tDeltaX;
				tMax = tMaxX;
				posX += stepX;
			}
			else
			{
				posVector = ray_position + ray_direction * tMax;
				tMaxZ += tDeltaZ;
				tMax = tMaxZ;
				posZ += stepZ;
			}
			/*Check if ray is outside of the grid*/
			if (posX >= grid_resolution || posX < 0 ||
				posZ >= grid_resolution || posZ < 0)
			{
				return glm::zero<glm::uvec3>();
			}

			/*Check if ray intersects*/
			index = posX * grid_resolution + posZ;
			if (point_buffer[index] >= posVector.y)
			{
				float factor = point_buffer[index] / (maxHeight - minHeight);
				return glm::uvec3(255*factor, 0, 0);
			}
		}
	}


	/*
	 * Start the ray tracing algorithm for each pixel
	 */
	__global__ void cuda_rayTrace(unsigned char* colorBuffer)
	{
		/*2D Grid and Block*/
		/*int blockId = blockIdx.x + blockIdx.y * gridDim.x;
		int threadId = blockId * (blockDim.x * blockDim.y) + (threadIdx.y * blockDim.x) + threadIdx.x;*/

		/*1D Grid and Block*/
		/*int threadId = blockIdx.x *blockDim.x + threadIdx.x;*/

		/*2D Grid and 1D Block*/
		int threadId = blockIdx.y * gridDim.x + blockIdx.x;

		glm::uvec3 color;
		glm::vec3 ray_position, ray_direction, up, right;

		right = glm::cross(*camera_forward, glm::vec3(0,1,0));
		up = glm::cross(right, *camera_forward);

		ray_position = *grid_camera_position +
			*camera_forward * frame_dimension->z +
			(blockIdx.y * blockDim.y + threadIdx.y - (texture_resolution->y - 1)/ 2.0f) * up +
			(blockIdx.x * blockDim.x + threadIdx.x - (texture_resolution->x - 1)/ 2.0f) * right;

		ray_direction = glm::normalize(ray_position - *grid_camera_position);
		color = castRay(ray_position, ray_direction, threadId);

		//GL_BGRA
		colorBuffer[threadId * 3] = static_cast<unsigned char>(color.r);
		colorBuffer[threadId * 3 + 1] = static_cast<unsigned char>(color.g);
		colorBuffer[threadId * 3 + 2] = static_cast<unsigned char>(color.b);
	}

	/*
	* Set device parameters
	* TODO: replace with a single Memcpy call?
	*/
	__global__ void cuda_setParameters(int grid_res, glm::ivec2 texture_res, glm::vec3 frame_dim, glm::vec3 camera_for, glm::vec3 camera_rig, float camera_height, float* d_gpu_pointBuffer, float minH, float maxH)
	{
		grid_resolution = grid_res;
		point_buffer = d_gpu_pointBuffer;
		minHeight = minH;
		maxHeight = maxH;
		*frame_dimension = frame_dim;
		*camera_forward = camera_for;
		*camera_right = camera_rig;
		*grid_camera_position = glm::vec3(grid_resolution / 2.0f, camera_height, grid_resolution / 2.0f);
		*texture_resolution = texture_res;
	}
	__global__ void cuda_initializeDeviceVariables()
	{
		frame_dimension = static_cast<glm::vec3*>(malloc(sizeof(glm::vec3)));
		camera_forward = static_cast<glm::vec3*>(malloc(sizeof(glm::vec3)));
		camera_right = static_cast<glm::vec3*>(malloc(sizeof(glm::vec3)));
		grid_camera_position = static_cast<glm::vec3*>(malloc(sizeof(glm::vec3)));
		texture_resolution = static_cast<glm::ivec2*>(malloc(sizeof(glm::ivec2)));
	}
	__global__ void cuda_freeDeviceVariables()
	{
		free(camera_forward);
		free(camera_right);
		free(grid_camera_position);
		free(texture_resolution);
		free(frame_dimension);
	}

	/*
	 * Set grid and block dimensions, pass parameters to device and call kernels
	 */
	__host__ void rayTrace(glm::ivec2& texture_resolution, glm::vec3& frame_dimensions, glm::vec3& camera_forward, glm::vec3& camera_right, float camera_height, unsigned char* colorBuffer, float* d_gpu_pointBuffer, int gpu_grid_res, float minHeight, float maxHeight)
	{
		//TODO: optimize Grid and Block sizes
		dim3 gridSize(texture_resolution.x, texture_resolution.y);
		dim3 blockSize(1);

		cuda_setParameters << <1, 1 >> > (gpu_grid_res, texture_resolution, frame_dimensions, camera_forward, camera_right, camera_height, d_gpu_pointBuffer, minHeight, maxHeight);
		checkCudaErrors(cudaDeviceSynchronize());
		cuda_rayTrace << <gridSize, blockSize >> > (colorBuffer);
		checkCudaErrors(cudaDeviceSynchronize());
	}

	__host__ void initializeDeviceVariables()
	{
		cuda_initializeDeviceVariables << <1, 1 >> > ();
		checkCudaErrors(cudaDeviceSynchronize());
	}

	__host__ void freeDeviceVariables()
	{
		cuda_freeDeviceVariables << <1, 1 >> > ();
		checkCudaErrors(cudaDeviceSynchronize());
	}
}
