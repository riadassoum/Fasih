// fasih-us-dump VOICE UTT OUT.raw : one whole recording as decoded by the engine (float32)
#include "us_core.hpp"
#include <cstdio>
#include <vector>
#include <cstdlib>
int main(int c,char**v){ fasih_us::voice V(v[1]); uint32_t u=atoi(v[2]); auto a=V.utt_start(u),b=V.utt_end(u);
 std::vector<float> x(b-a); V.read(u,a,b,x.data()); FILE*f=fopen(v[3],"wb"); fwrite(x.data(),4,x.size(),f); fclose(f);}
