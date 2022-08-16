```
apt install git-filter-repo

mkdir combine
cd combine
git clone git@github.com:RadeonOpenCompute/ROCm-OpenCL-Runtime.git
git clone git@github.com:ROCm-Developer-Tools/ROCclr.git
git clone git@github.com:ROCm-Developer-Tools/hipamd.git
#change to your repo
git clone git@github.com:saadrahim/triple.git


cd hipamd
git filter-repo --to-subdirectory-filter hipamd
cd ..
cd triple
git remote add hipamd ../hipamd/
git fetch hipamd --tags
git merge --allow-unrelated-histories hipamd/develop
git remote remove hipamd

cd ..
cd ROCclr
git filter-repo --to-subdirectory-filter ROCclr
cd ..
cd triple
git remote add ROCclr ../ROCclr/
git fetch ROCclr --tags
git merge --allow-unrelated-histories ROCclr/develop

cd ..
cd ROCm-OpenCL-Runtime
git filter-repo --to-subdirectory-filter ROCm-OpenCL-Runtime
cd ..
cd triple
git remote add ROCm-OpenCL-Runtime ../ROCm-OpenCL-Runtime/
git fetch ROCm-OpenCL-Runtime --tags
git merge --allow-unrelated-histories ROCm-OpenCL-Runtime/develop

```
