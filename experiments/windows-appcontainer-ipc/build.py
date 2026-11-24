import argparse,subprocess,sys
from pathlib import Path
root=Path.cwd();sys.path.insert(0,str(root/'scripts'))
from desktop import environment
parser=argparse.ArgumentParser();parser.add_argument('--vcvars',required=True);args=parser.parse_args()
env=environment(args.vcvars)
source=root/'experiments/windows-appcontainer-ipc'; build=root/'build/cycle-268-appcontainer/out'
cmake=root/'build/toolchain/desktop-tools/Scripts/cmake.exe';ninja=root/'build/toolchain/desktop-tools/Scripts/ninja.exe';qt=root/'build/Qt/6.8.2/msvc2022_64'
subprocess.run([str(cmake),'-S',str(source),'-B',str(build),'-G','Ninja','-DCMAKE_BUILD_TYPE=Release','-DCMAKE_MAKE_PROGRAM='+str(ninja),'-DCMAKE_PREFIX_PATH='+str(qt)],env=env,check=True)
subprocess.run([str(cmake),'--build',str(build),'--parallel','2'],env=env,check=True)
