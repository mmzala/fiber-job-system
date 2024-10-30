#include <Windows.h>
#include <iostream>
#include <vector>
#include <cmath>
#include <random>
#include <algorithm>
#include <chrono>

#include "JobSystem.h"

JobSystem jobSystem{};
std::atomic<uint32_t> fibonacciIterations = 0;

void CalculateFibonacci(void* param) 
{
    int32_t* pNumber = reinterpret_cast<int32_t*>(param);
    int32_t n = *pNumber;

    if (n > 1) 
    {
        int32_t fibMinus1 = n - 1;
        int32_t fibMinus2 = n - 2;

        Counter counter{};
        JobDecl jobs[2];
        jobs[0].mFunction = CalculateFibonacci;
        jobs[0].mParam = &fibMinus1;
        jobs[1].mFunction = CalculateFibonacci;
        jobs[1].mParam = &fibMinus2;

        jobSystem.RunJobs(jobs, 2, &counter);
        jobSystem.WaitForCounter(&counter);

        n = fibMinus1 + fibMinus2;
        *pNumber = n;
    }

    fibonacciIterations++;
}

void VectorSort(void* param = nullptr)
{
    // Initialize random number generator
    std::mt19937 gen(383628);
    std::uniform_real_distribution<> dis(0.0, 1.0);

    // Create a vector of random doubles
    std::vector<double> data(900);
    for (auto& elem : data) 
    {
        elem = dis(gen);
    }

    // Perform computationally expensive operations
    for (size_t i = 0; i < 900; ++i) 
    {
        double sum = 0.0;

        for (size_t j = 0; j < 900; ++j) 
        {
            sum += std::sin(data[j]) * std::cos(data[(i + j) % 900]);
        }

        data[i] = std::exp(std::fabs(sum));
    }

    // Sort the vector to add more computation and prevent easy optimization
    std::sort(data.begin(), data.end());
}

void MainFiber(void* system)
{
    std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();

    JobSystem& jobSystem = *reinterpret_cast<JobSystem*>(system);

    int32_t fibonacci = 13;
    fibonacciIterations = 0;

    JobDecl fibonacciJob = JobDecl{ CalculateFibonacci, &fibonacci };

    Counter counter{};
    jobSystem.RunJobs(&fibonacciJob, 1, &counter);
    jobSystem.WaitForCounter(&counter);

    std::cout << "Fibonacci job done with " << fibonacciIterations << " iterations\n";

    const uint32_t numVectorJobs = 100;
    JobDecl vectorJobs[numVectorJobs];
    
    for (int i = 0; i < numVectorJobs; ++i)
    {
        vectorJobs[i] = JobDecl{ VectorSort };
    }

    jobSystem.RunJobs(vectorJobs, numVectorJobs, &counter); // We can reuse the counter
    jobSystem.WaitForCounter(&counter);

    std::cout << numVectorJobs << " Jobs done. Left: " << counter << " jobs\n";

    std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();

    std::cout << "Run time : " << std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count() << " ms\n";
}

int main()
{
    std::cout << "Program running with " << std::thread::hardware_concurrency() << " threads\n";

    Counter counter{};
    JobDecl job{ MainFiber, &jobSystem };
    jobSystem.RunJobs(&job, 1, &counter);
    
    jobSystem.Join();
    jobSystem.ShutDown();

	return 0;
}