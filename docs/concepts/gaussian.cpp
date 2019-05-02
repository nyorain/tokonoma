// https://stackoverflow.com/questions/8204645
// see stackoverflow for license information

#include <cmath>
#include <vector>
#include <iostream>
#include <iomanip>

double gaussian( double x, double mu, double sigma ) {
    const double a = ( x - mu ) / sigma;
    return std::exp( -0.5 * a * a );
}

typedef std::vector<double> kernel_row;
typedef std::vector<kernel_row> kernel_type;

kernel_type produce2dGaussianKernel (int kernelRadius) {
  double sigma = kernelRadius/2.;
  kernel_type kernel2d(2*kernelRadius+1, kernel_row(2*kernelRadius+1));
  double sum = 0;
  // compute values
  for (unsigned row = 0; row < kernel2d.size(); row++)
    for (unsigned col = 0; col < kernel2d[row].size(); col++) {
      double x = gaussian(row, kernelRadius, sigma)
               * gaussian(col, kernelRadius, sigma);
      kernel2d[row][col] = x;
      sum += x;
    }
  // normalize
  for (unsigned row = 0; row < kernel2d.size(); row++)
    for (unsigned col = 0; col < kernel2d[row].size(); col++)
      kernel2d[row][col] /= sum;
  return kernel2d;
}

int main() {
  kernel_type kernel2d = produce2dGaussianKernel(4);
  std::cout << std::setprecision(5) << std::fixed;
  for (unsigned row = 0; row < kernel2d.size(); row++) {
    for (unsigned col = 0; col < kernel2d[row].size(); col++)
      std::cout << kernel2d[row][col] << ", ";
    std::cout << "},\n{";
  }
}
