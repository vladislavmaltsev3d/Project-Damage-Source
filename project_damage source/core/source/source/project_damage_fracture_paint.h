// общие объявления: привязываем трещины и ответвления к нарисованным линиям
// SPDX-License-Identifier: GPL-3.0-or-later
// Internal painted fracture path. Keep stroke placement independent of the
// procedural surface walker: relief edges are not endpoints of a drawn crack.
struct FrPaintSurface
{
    const FrTriMesh& mesh;
    const std::vector<bool>* avoid;
    const IFieldMask* mask;
    ProjectDamageBvhTree tree;
    std::vector<Int32> component;

    FrPaintSurface(const FrTriMesh& m, const std::vector<bool>* a, const IFieldMask* field):mesh(m),avoid(a),mask(field)
    {
        std::vector<ProjectDamageBvhTree::AABB> boxes(mesh.tris.size());
        for(size_t ti=0;ti<mesh.tris.size();++ti)
            for(Int32 v:mesh.tris[ti].v)boxes[ti].Grow(mesh.verts[v]);
        tree.Build(boxes);
        component.assign(mesh.tris.size(),-1);
        for(size_t ti=0;ti<mesh.tris.size();++ti)
        {
            if(component[ti]>=0)continue;
            std::vector<Int32> todo{Int32(ti)};component[ti]=Int32(ti);
            while(!todo.empty())
            {
                Int32 t=todo.back();todo.pop_back();
                for(Int32 n:mesh.tris[t].nbr)
                    if(n>=0&&component[n]<0){component[n]=Int32(ti);todo.push_back(n);}
            }
        }
    }

    bool Allowed(Int32 t)const
    {
        return t>=0&&size_t(t)<mesh.tris.size()&&(!avoid||!(*avoid)[t])
            &&m::la::length(mesh.nrm[t])>.5;
    }

    Int32 Nearest(const m::vec3& p,double reach,m::vec3& q,Int32 shell=-1)const
    {
        if(tree.Empty())return -1;
        double best=reach*reach;Int32 found=-1;
        std::vector<Int32> todo{0};
        while(!todo.empty())
        {
            const auto& node=tree.nodes[todo.back()];todo.pop_back();
            const m::vec3 box=m::la::max(node.box.lo,m::la::min(node.box.hi,p));
            const auto delta=p-box;
            if(m::la::dot(delta,delta)>best)continue;
            if(!node.IsLeaf()){todo.push_back(node.left);todo.push_back(node.right);continue;}
            for(Int32 k=node.start;k<node.start+node.count;++k)
            {
                Int32 ti=tree.order[k];
                if(!Allowed(ti)||(shell>=0&&component[ti]!=shell))continue;
                const auto& t=mesh.tris[ti];
                const auto a=mesh.verts[t.v[0]],b=mesh.verts[t.v[1]],c=mesh.verts[t.v[2]],n=mesh.nrm[ti];
                m::vec3 hit=p-n*m::la::dot(p-a,n);
                if(m::la::dot(m::la::cross(b-a,hit-a),n)<-1e-10
                   ||m::la::dot(m::la::cross(c-b,hit-b),n)<-1e-10
                   ||m::la::dot(m::la::cross(a-c,hit-c),n)<-1e-10)
                {
                    hit=FrClosestOnSeg(p,a,b);
                    for(const auto& e:std::array<std::pair<m::vec3,m::vec3>,2>{{{b,c},{c,a}}})
                    {
                        auto v=FrClosestOnSeg(p,e.first,e.second);
                        if(m::la::dot(v-p,v-p)<m::la::dot(hit-p,hit-p))hit=v;
                    }
                }
                const auto d=hit-p;double d2=m::la::dot(d,d);
                if(d2<best){best=d2;found=ti;q=hit;}
            }
        }
        return found;
    }

                                                                            
                                                                   
    std::vector<FrCrack> Project(const std::vector<EVec>& path,double step,
        double reach,Int32 gen,double width)const
    {
        std::vector<FrCrack> runs;FrCrack c;
        auto finish=[&](){
            if(c.pts.size()>=2){c.gen=gen;c.guided=true;c.noSpall=true;c.hit=true;
                c.widthOverride=width;runs.push_back(std::move(c));}
            c=FrCrack();
        };
        auto sample=[&](const m::vec3& p){
            m::vec3 q;Int32 t=Nearest(p,reach,q);
            if(t<0||(mask&&mask->FadeScale(q)<=1e-6)){finish();return;}
            if(!c.pts.empty())
            {
                const double d=m::la::length(q-c.pts.back());
                if(d<1e-6)return;
                if(component[t]!=component[c.tris.back()]||d>MaxD(4.0*step,.4*reach))finish();
            }
            c.pts.push_back(q);c.nrms.push_back(mesh.nrm[t]);c.tris.push_back(t);
        };
        if(path.empty())return runs;
        sample(path.front());
        for(size_t i=1;i<path.size();++i)
        {
            const double len=m::la::length(path[i]-path[i-1]);
            const int count=std::max(1,int(std::ceil(len/step)));
            for(int j=1;j<=count;++j)sample(path[i-1]+(path[i]-path[i-1])*(double(j)/count));
        }
        finish();return runs;
    }
};

// Cutter frames for a painted spine. Placement is untouched: every kept
// sample is a projected surface point and both ends (the junctions with the
// parent crack) stay exactly where they are. Only the cross-section frames
// change. The raw samples carry the normal of whichever triangle they landed
// on, so on a retopo or sculpt the section flips by the facet angle at every
// triangle edge, and their tangent is a two-sample difference of projected
// points. Both are now averaged over a short arc window (a tent weight over
// `radius`), and samples closer than `minGap` are dropped so a tangent is
// never taken over a near-zero chord.
void FrPaintFrames(FrCrack& c,double minGap,double radius,std::vector<m::vec3>& tangents)
{
    std::vector<m::vec3> P,N;
    P.reserve(c.pts.size());N.reserve(c.pts.size());
    for(size_t i=0;i<c.pts.size();++i)
    {
        const bool last=i+1==c.pts.size();
        if(!P.empty()&&!last&&m::la::length(c.pts[i]-P.back())<minGap)continue;
        if(last&&P.size()>1&&m::la::length(c.pts[i]-P.back())<minGap){P.back()=c.pts[i];N.back()=c.nrms[i];continue;}
        P.push_back(c.pts[i]);N.push_back(c.nrms[i]);
    }
    c.fP=P;c.fN=N;tangents.assign(P.size(),m::vec3(0.));
    if(P.size()<2){c.fP=c.pts;c.fN=c.nrms;tangents.clear();return;}
    std::vector<double> s(P.size(),0.);
    for(size_t i=1;i<P.size();++i)s[i]=s[i-1]+m::la::length(P[i]-P[i-1]);
    const double r=MaxD(radius,1e-9),rt=MaxD(.5*radius,1e-9);
    size_t lo=0,hi=0;
    for(size_t i=0;i<P.size();++i)
    {
        while(s[i]-s[lo]>r)++lo;
        while(hi+1<P.size()&&s[hi+1]-s[i]<=r)++hi;
        m::vec3 n(0.);
        for(size_t j=lo;j<=hi;++j)n=n+N[j]*(1.-std::abs(s[j]-s[i])/r+1e-6);
        n=FrNorm(n);
        if(m::la::length(n)>.5&&m::la::dot(n,FrNorm(N[i]))>.2)c.fN[i]=n;
        // tangent: chord across the (shorter) tangent window
        size_t a=i,b=i;
        while(a>0&&s[i]-s[a-1]<=rt)--a;
        while(b+1<P.size()&&s[b+1]-s[i]<=rt)++b;
        if(a==b){a=i>0?i-1:i;b=i+1<P.size()?i+1:i;}
        tangents[i]=FrNorm(P[b]-P[a]);
    }
    for(size_t i=0;i<P.size();++i)
    {
        const auto t=tangents[i];
        auto target=FrNorm(c.fN[i]-t*m::la::dot(c.fN[i],t));
        if(m::la::length(target)<.5)target=FrNorm(m::la::cross(t,m::vec3(1.,.37,.19)));
        if(i>0)
        {
            const auto axis=m::la::cross(tangents[i-1],t);
            const double sine=m::la::length(axis),cosine=m::la::dot(tangents[i-1],t);
            auto transported=c.fN[i-1];
            if(sine>1e-9)transported=RotAxis(transported,axis/sine,std::atan2(sine,cosine));
            transported=FrNorm(transported-t*m::la::dot(transported,t));
            if(m::la::dot(transported,target)>.5)
            {
                const double blend=std::min(1.,2.*(s[i]-s[i-1])/r);
                target=FrNorm(transported*(1.-blend)+target*blend);
            }
        }
        c.fN[i]=target;
    }
}

void FrPlacePainted(MeshView meshView,const FrTriMesh& mesh,const ProjectDamageParams& params,
    const ProjectDamageHost& host,const std::vector<bool>* avoidTri,
    const std::vector<std::vector<EVec>>& guides,const std::vector<Int32>* levels,double ns,
    std::vector<m::Manifold>& cutters,std::vector<Int32>& cutterLevels)
{
    FrPaintSurface surface(mesh,avoidTri,params.mask);
    std::vector<FrCrack> cracks;
    const double detail=MaxD(params.fractDetail,.001);
    auto stepFor=[&](int level){
        double step=std::min(FR_CUT_STEP*ns/detail,MaxD(.04,params.fract[level].width)*.4);
        const auto& lvl=params.fract[level];
        if(level>0&&lvl.paintDistortion>0.)step=std::min(step,lvl.paintDistortionScale/12.);
        return MaxD(.02,step);
    };
    for(size_t gi=0;gi<guides.size();++gi)
    {
        int gen=levels&&gi<levels->size()?std::max(1,std::min(3,(*levels)[gi]+1)):1;
        const auto& lvl=params.fract[gen-1];
        if(lvl.width<=0.||lvl.depth<=0.)continue;
        const double radius=MaxD(lvl.paintRadius,.04);
        auto runs=surface.Project(guides[gi],stepFor(gen-1),radius,gen,std::min(lvl.width,radius/1.3));
        for(auto& run:runs)cracks.push_back(std::move(run));
    }
    const size_t primary=cracks.size();
    if(cracks.empty())
    {
        HostLog(host,"[ProjectDamage] Paint Fracture: no stroke within Paint Radius of an allowed surface");
        return;
    }
    FrHash hash;hash.cell=MaxD(.1,FR_ATTRACT_R*ns);
    std::vector<FrEntry> points;
    for(size_t i=0;i<cracks.size();++i)FrRegisterCrack(hash,points,cracks[i],Int32(i));
    Int32 placed[2]={0,0};
    for(int gen=2;gen<=3;++gen)
    {
        const auto& lvl=params.fract[gen-1];
        if(lvl.amount<=0||lvl.width<=0.||lvl.depth<=0.)continue;
        std::vector<size_t> parents;std::vector<double> cdf;double sum=0.;
        for(size_t i=0;i<cracks.size();++i)
        {
            if(cracks[i].gen!=gen-1)continue;
            double length=0.;for(size_t j=1;j<cracks[i].pts.size();++j)length+=m::la::length(cracks[i].pts[j]-cracks[i].pts[j-1]);
            if(length<=1e-6)continue;
            parents.push_back(i);cdf.push_back(sum+=length);
        }
        if(parents.empty())continue;
        PyRandom rng(uint32_t(params.seed+7020+10*gen));
        const double width=std::min(lvl.width,MaxD(.04,lvl.paintRadius)/1.3),step=stepFor(gen-1);
        const double snap=MaxD(.03,width*.35),blind=MaxD(3.*width,4.*step);
        auto grow=[&](Int32 parent,Int32 tri,m::vec3 pos,m::vec3 dir,double limit,PyRandom& rng,FrCrack& branch){
            const auto normal=mesh.nrm[tri];m::vec3 q;FrEntry nearby;double nearest=0.;
            branch.gen=gen;branch.bornOn=Int32(parent);branch.noSpall=true;branch.widthOverride=width;
            branch.pts.push_back(pos);branch.nrms.push_back(normal);branch.tris.push_back(tri);
                                                                          
                                                                            
                                                                             
                                                                         
            const uint64_t shapeSeed=uint64_t(uint32_t(params.seed))+uint64_t(parent+1)*7919u
                +uint64_t(gen)*104729u+uint64_t(cracks.size()+1)*15485863u;
            const double phase1=2.*m::kPi*Grain01(shapeSeed,0,0);
            const double phase2=2.*m::kPi*Grain01(shapeSeed,0,1);
            const double phase3=2.*m::kPi*Grain01(shapeSeed,0,2);
            const double wavelength=MaxD(lvl.paintDistortionScale,4.*step);
            auto heading=[&](double arc){
                const double x=2.*m::kPi*arc/wavelength;
                return .9*MaxD(0.,lvl.paintDistortion)*(.65*std::sin(x+phase1)
                    +.25*std::sin(2.3*x+phase2)+.1*std::sin(5.1*x+phase3));
            };
            double lastHeading=heading(0.),length=0.;
            while(length<limit&&branch.pts.size()<10000)
            {
                const double h=std::min(step,limit-length);Int32 nextTri=tri;
                m::vec3 next=pos,d=dir;
                if(lvl.paintDistortion<0.)
                    d=RotAxis(d,mesh.nrm[tri],FrGauss(rng,.025*params.fractDispStrength));
                else
                {
                    const double nextHeading=heading(length+h);
                    d=RotAxis(d,mesh.nrm[tri],nextHeading-lastHeading);
                    lastHeading=nextHeading;
                }
                bool marched=FrMarch(mesh,nextTri,next,d,h);
                if(!marched||!surface.Allowed(nextTri)||surface.component[nextTri]!=surface.component[tri])break;
                                                                            
                                                                            
                nextTri=surface.Nearest(next,MaxD(.03,h*.2),q,surface.component[tri]);
                if(nextTri<0||(params.mask&&params.mask->FadeScale(q)<=1e-6))break;
                next=q;double advance=m::la::length(next-pos);
                if(advance<1e-6||advance>3.*h)break;
                if(length>blind&&hash.Nearest(next,snap,&mesh.nrm[nextTri],
                    [&](const FrEntry& e){return e.crack==Int32(parent);},nearby,nearest))
                {next=FrJunctionPoint(next,nearby,cracks);branch.hit=true;branch.hitCrack=nearby.crack;}
                branch.pts.push_back(next);branch.nrms.push_back(mesh.nrm[nextTri]);branch.tris.push_back(nextTri);
                length+=advance;pos=next;tri=nextTri;dir=d;
                if(branch.hit)break;
            }
            return branch.pts.size()>=3&&length>=MaxD(.04,width*.4);
        };
        int attempts=0;
        while(placed[gen-2]<lvl.amount&&attempts++<lvl.amount*24)
        {
            double pick=rng.Random()*sum;
            size_t pi=std::min(parents.size()-1,size_t(std::upper_bound(cdf.begin(),cdf.end(),pick)-cdf.begin()));
            const size_t parent=parents[pi];const FrCrack& p=cracks[parent];
            const double parentLength=cdf[pi]-(pi?cdf[pi-1]:0.);
            double arc=rng.Uniform(.05,.95)*parentLength;
            size_t segment=0;
            while(segment+2<p.pts.size()&&arc>m::la::length(p.pts[segment+1]-p.pts[segment]))
            {arc-=m::la::length(p.pts[segment+1]-p.pts[segment]);++segment;}
            m::vec3 tangent=FrNorm(p.pts[segment+1]-p.pts[segment]);
            m::vec3 pos=p.pts[segment]+tangent*arc,q;
            Int32 tri=surface.Nearest(pos,MaxD(step,width),q,surface.component[p.tris[segment]]);
            if(tri<0||(params.mask&&params.mask->FadeScale(q)<=1e-6))continue;
            pos=q;m::vec3 normal=mesh.nrm[tri];
            m::vec3 dir=FrNorm(m::la::cross(normal,tangent));
            if(m::la::length(dir)<.5)continue;
            if(rng.Random()<.5)dir=dir*-1.;
            dir=RotAxis(dir,normal,rng.Uniform(-.3,.3));
            FrEntry nearby;double nearest=0.;
            if(hash.Nearest(pos,MaxD(width,step)*1.2,&normal,
                [&](const FrEntry& e){return e.crack==Int32(parent);},nearby,nearest))continue;
            const double defaultLength=std::max(8.*width,std::min(parentLength*.35,3.*MaxD(lvl.paintRadius,.04)));
            double limit=(lvl.length>0.?lvl.length:defaultLength)*rng.Uniform(.65,1.);
            if(lvl.paintLengthMin>=0. && lvl.length>0.)
            {
                const double sample=limit/lvl.length;
                const double t=(sample-.65)/.35;
                limit=lvl.paintLengthMin+(lvl.length-lvl.paintLengthMin)*std::clamp(t,0.,1.);
            }
            FrCrack branch;
            if(!grow(Int32(parent),tri,pos,dir,limit,rng,branch))continue;
            FrRegisterCrack(hash,points,branch,Int32(cracks.size()));cracks.push_back(std::move(branch));++placed[gen-2];
        }
                                                                            
                                                                           
        const double per=MaxD(0.,lvl.branches);
        if(per>0.&&lvl.branchLength>0.)
        {
            PyRandom forkRng(uint32_t(params.seed+7040+gen));
            const size_t parentCount=cracks.size();
            for(size_t ci=0;ci<parentCount;++ci)
            {
                const FrCrack par=cracks[ci];
                if(par.gen!=gen||par.guided||par.isBranch)continue;
                const int count=int(std::floor(per))+(forkRng.Random()<per-std::floor(per)?1:0);
                double total=0.;for(size_t j=1;j<par.pts.size();++j)total+=m::la::length(par.pts[j]-par.pts[j-1]);
                for(int k=0;k<count;++k)
                {
                    double at=forkRng.Uniform(.15,.85)*total;size_t j=0;
                    while(j+2<par.pts.size()&&at>m::la::length(par.pts[j+1]-par.pts[j]))
                    {at-=m::la::length(par.pts[j+1]-par.pts[j]);++j;}
                    auto tangent=FrNorm(par.pts[j+1]-par.pts[j]);
                    m::vec3 q;Int32 tri=surface.Nearest(par.pts[j]+tangent*at,MaxD(width,step),q,surface.component[par.tris[j]]);
                    if(tri<0||(params.mask&&params.mask->FadeScale(q)<=1e-6))continue;
                    double angle=forkRng.Uniform(18.,40.)*m::kPi/180.;
                    if(forkRng.Random()<.5)angle=-angle;
                    auto dir=RotAxis(tangent,mesh.nrm[tri],angle);
                    double limit=total*lvl.branchLength*forkRng.Uniform(.6,1.25);
                    if(lvl.length>0.)limit=std::min(limit,lvl.length);
                    FrCrack branch;if(!grow(Int32(ci),tri,q,dir,limit,forkRng,branch))continue;
                    branch.isBranch=true;
                    FrRegisterCrack(hash,points,branch,Int32(cracks.size()));cracks.push_back(std::move(branch));
                }
            }
        }
    }
    ProjectDamageBVH bvh;
    const bool cap=params.fractRecipe>=2&&params.fractThicknessCap>0.;
    if(cap)bvh.Build(meshView);
    for(size_t i=0;i<cracks.size();++i)
    {
        auto& c=cracks[i];
        const auto& lvl=params.fract[c.gen-1];const size_t before=cutters.size();
        const double step=stepFor(c.gen-1),w=MaxD(c.widthOverride,1e-6);
        std::vector<m::vec3> tangents;
        FrPaintFrames(c,.5*step,MaxD(4.*w,6.*step),tangents);
        PyRandom rng(uint32_t(params.seed+7101+7919u*uint32_t(i)));
        FrBuildCrackCutters(c,c.widthOverride,lvl.depth*params.fractDepthMult,detail,ns,rng,
            cutters,params.mask,params.fractRecipe>=2?2:1,cap?&bvh:nullptr,params.fractThicknessCap,
            params.fractRimJag,params.fractFlare,tangents.empty()?nullptr:&tangents,2.*step);
        for(size_t j=before;j<cutters.size();++j)cutterLevels.push_back(c.gen-1);
    }
    HostLog(host,"[ProjectDamage] Paint Fracture: "+std::to_string(primary)+" surface runs, Level 2 "
        +std::to_string(placed[0])+"/"+std::to_string(params.fract[1].amount)+", Level 3 "
        +std::to_string(placed[1])+"/"+std::to_string(params.fract[2].amount));
}
