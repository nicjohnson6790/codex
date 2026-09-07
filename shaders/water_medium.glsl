// Stable homogeneous single-scattering integral, including zero extinction.
// Returns integral sigmaS * exp(-sigmaT*s) ds, per RGB channel.
float waterScatteringIntegral(float sigmaS, float sigmaT, float distance)
{
    float tau = sigmaT * distance;
    float average = tau < 0.001f
        ? 1.0f-tau*0.5f+tau*tau/6.0f
        : (1.0f-exp(-tau))/tau;
    return sigmaS * distance * average;
}
