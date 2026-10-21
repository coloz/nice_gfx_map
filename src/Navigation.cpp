#include "Navigation.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <utility>

namespace nice_gfx_map {
namespace {
constexpr double Pi = 3.1415926535897932384626433832795;
constexpr double EarthRadius = 6371008.8;
constexpr double Radians = Pi / 180.0;
double nan() { return std::numeric_limits<double>::quiet_NaN(); }
double clampUnit(double value) { return std::max(-1.0, std::min(1.0, value)); }
double heading(double value) {
  value = std::fmod(value, 360.0);
  return value < 0 ? value + 360.0 : value;
}
GeoPoint normalized(GeoPoint point) { point.longitude = normalizeLongitude(point.longitude); return point; }
struct Vector { double x, y, z; };
Vector vector(GeoPoint point) {
  const double lat = point.latitude * Radians;
  const double lon = normalizeLongitude(point.longitude) * Radians;
  const double c = std::cos(lat);
  return {c * std::cos(lon), c * std::sin(lon), std::sin(lat)};
}
Vector cross(Vector a, Vector b) { return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; }
double dot(Vector a, Vector b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
double norm(Vector a) { return std::sqrt(dot(a,a)); }
Vector scaled(Vector a, double scale) { return {a.x*scale, a.y*scale, a.z*scale}; }
double angle(Vector a, Vector b) { return std::atan2(norm(cross(a,b)), clampUnit(dot(a,b))); }
GeoPoint coordinate(Vector v) { return {std::atan2(v.z,std::hypot(v.x,v.y))/Radians,
                                       normalizeLongitude(std::atan2(v.y,v.x)/Radians)}; }
struct PlanePoint { double x, y; };
struct Plane {
  Vector center, east, north;
  explicit Plane(GeoPoint pivot) : center(vector(pivot)) {
    east = cross({0,0,1},center);
    if (norm(east) < 1e-8) east = cross({1,0,0},center);
    east = scaled(east,1/norm(east)); north = cross(center,east);
  }
  bool project(GeoPoint point, PlanePoint &out) const {
    const Vector v = vector(point); const double divisor = dot(v,center);
    if (divisor <= 1e-8) return false;
    out = {dot(v,east)/divisor,dot(v,north)/divisor}; return true;
  }
};
double turn(PlanePoint a, PlanePoint b, PlanePoint c) {
  return (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);
}
bool intersects(PlanePoint a, PlanePoint b, PlanePoint c, PlanePoint d, double epsilon) {
  const double abC=turn(a,b,c), abD=turn(a,b,d), cdA=turn(c,d,a), cdB=turn(c,d,b);
  if (((abC>epsilon && abD < -epsilon)||(abC < -epsilon && abD>epsilon)) &&
      ((cdA>epsilon && cdB < -epsilon)||(cdA < -epsilon && cdB>epsilon))) return true;
  const double linearEpsilon = std::sqrt(std::max(epsilon,0.0))*1e-3;
  const auto on = [&](PlanePoint p,PlanePoint q,PlanePoint r,double area) {
    return std::abs(area)<=epsilon && r.x>=std::min(p.x,q.x)-linearEpsilon &&
        r.x<=std::max(p.x,q.x)+linearEpsilon && r.y>=std::min(p.y,q.y)-linearEpsilon &&
        r.y<=std::max(p.y,q.y)+linearEpsilon;
  };
  return on(a,b,c,abC)||on(a,b,d,abD)||on(c,d,a,cdA)||on(c,d,b,cdB);
}
} // namespace

double distanceMeters(GeoPoint a, GeoPoint b) {
  if (!validCoordinate(a)||!validCoordinate(b)) return nan();
  const double latA=a.latitude*Radians,latB=b.latitude*Radians;
  const double dLat=(b.latitude-a.latitude)*Radians;
  const double dLon=normalizeLongitude(normalizeLongitude(b.longitude)-normalizeLongitude(a.longitude))*Radians;
  const double sLat=std::sin(dLat/2),sLon=std::sin(dLon/2);
  const double value=std::max(0.0,std::min(1.0,sLat*sLat+std::cos(latA)*std::cos(latB)*sLon*sLon));
  return EarthRadius*2*std::atan2(std::sqrt(value),std::sqrt(1-value));
}

double initialBearingDegrees(GeoPoint a, GeoPoint b) {
  if (!validCoordinate(a)||!validCoordinate(b)) return nan();
  const Vector av=vector(a),bv=vector(b);
  if (norm(cross(av,bv))<1e-14) return dot(av,bv)>=0 ? 0.0 : nan();
  const double latA=a.latitude*Radians,latB=b.latitude*Radians;
  const double dLon=normalizeLongitude(normalizeLongitude(b.longitude)-normalizeLongitude(a.longitude))*Radians;
  return heading(std::atan2(std::sin(dLon)*std::cos(latB),
      std::cos(latA)*std::sin(latB)-std::sin(latA)*std::cos(latB)*std::cos(dLon))/Radians);
}

GeoPoint destinationPoint(GeoPoint start, double bearingDegrees, double meters) {
  if (!validCoordinate(start)||!std::isfinite(bearingDegrees)||!std::isfinite(meters)||meters<0)
    return {nan(),nan()};
  const double lat=start.latitude*Radians,lon=normalizeLongitude(start.longitude)*Radians;
  const double bearing=heading(bearingDegrees)*Radians,delta=std::fmod(meters/EarthRadius,2*Pi);
  const Vector origin=vector(start),east{-std::sin(lon),std::cos(lon),0};
  const Vector north{-std::sin(lat)*std::cos(lon),-std::sin(lat)*std::sin(lon),std::cos(lat)};
  const Vector direction{north.x*std::cos(bearing)+east.x*std::sin(bearing),
      north.y*std::cos(bearing)+east.y*std::sin(bearing),
      north.z*std::cos(bearing)+east.z*std::sin(bearing)};
  // A tangent-vector step also works at the exact poles, where the usual
  // longitude atan2 formula suffers cancellation in both arguments.
  return coordinate({origin.x*std::cos(delta)+direction.x*std::sin(delta),
      origin.y*std::cos(delta)+direction.y*std::sin(delta),
      origin.z*std::cos(delta)+direction.z*std::sin(delta)});
}

double distanceToSegmentMeters(GeoPoint point, GeoPoint a, GeoPoint b) {
  if (!validCoordinate(point)||!validCoordinate(a)||!validCoordinate(b)) return nan();
  const Vector p=vector(point),av=vector(a),bv=vector(b);
  Vector normal=cross(av,bv); const double length=norm(normal);
  const double endpoints=std::min(distanceMeters(point,a),distanceMeters(point,b));
  if (length<1e-12) return endpoints;
  normal=scaled(normal,1/length);
  const double perpendicular=dot(p,normal);
  Vector projected{p.x-normal.x*perpendicular,p.y-normal.y*perpendicular,p.z-normal.z*perpendicular};
  const double projectedLength=norm(projected);
  if (projectedLength<1e-14) return endpoints;
  projected=scaled(projected,1/projectedLength);
  if (angle(av,projected)+angle(projected,bv)>angle(av,bv)+1e-10) return endpoints;
  return std::min(endpoints,EarthRadius*std::asin(std::abs(clampUnit(perpendicular))));
}

Status RouteNavigator::setRoute(const GeoPoint *points,size_t count,double arrivalRadiusMeters,double offRouteMeters) {
  if (!points||!count||!std::isfinite(arrivalRadiusMeters)||arrivalRadiusMeters<=0||
      !std::isfinite(offRouteMeters)||offRouteMeters<=0) return Status::InvalidArgument;
  if (count>MaximumPoints) return Status::TooLarge;
  for(size_t i=0;i<count;++i) if(!validCoordinate(points[i])) return Status::InvalidArgument;
  std::unique_ptr<GeoPoint[]> copy(new(std::nothrow) GeoPoint[count]);
  if(!copy) return Status::NoMemory;
  for(size_t i=0;i<count;++i) copy[i]=normalized(points[i]);
  std::unique_ptr<double[]> segments(count>1?new(std::nothrow) double[count-1]:nullptr);
  if(segments)for(size_t i=1;i<count;++i)segments[i-1]=distanceMeters(copy[i-1],copy[i]);
  points_=std::move(copy);segmentMeters_=std::move(segments);count_=count;next_=0;arrivalRadius_=arrivalRadiusMeters;offRoute_=offRouteMeters;
  return Status::Ok;
}

Status RouteNavigator::update(GeoPoint position,NavigationState &state) {
  if(!validCoordinate(position)) return Status::InvalidArgument;
  if(!count_) return Status::NotFound;
  size_t next=next_;
  while(next<count_&&distanceMeters(position,points_[next])<=arrivalRadius_) ++next;
  NavigationState result;
  result.arrived=next==count_;result.nextIndex=result.arrived?count_-1:next;
  result.metersToNext=distanceMeters(position,points_[result.nextIndex]);
  result.bearingToNext=initialBearingDegrees(position,points_[result.nextIndex]);
  if(!std::isfinite(result.bearingToNext)) result.bearingToNext=0;
  if(result.arrived) result.crossTrackMeters=result.metersToNext;
  else if(next) result.crossTrackMeters=distanceToSegmentMeters(position,points_[next-1],points_[next]);
  else result.crossTrackMeters=result.metersToNext;
  result.offRoute=!result.arrived&&result.crossTrackMeters>offRoute_;
  if(!result.arrived) {
    result.remainingMeters=result.metersToNext;
    // Preserve the original ordered additions, including floating-point
    // rounding; only fixed route-edge trigonometry is reused.
    for(size_t i=next+1;i<count_;++i) result.remainingMeters+=segmentMeters_?segmentMeters_[i-1]:distanceMeters(points_[i-1],points_[i]);
  }
  next_=next;state=result;return Status::Ok;
}

void RouteNavigator::reset() { points_.reset();segmentMeters_.reset();count_=next_=0; }

Status GeoFence::setCircle(GeoPoint center,double radiusMeters,double hysteresisMeters) {
  if(!validCoordinate(center)||!std::isfinite(radiusMeters)||radiusMeters<=0||radiusMeters>Pi*EarthRadius||
     !std::isfinite(hysteresisMeters)||hysteresisMeters<0||hysteresisMeters>Pi*EarthRadius)
    return Status::InvalidArgument;
  center_=normalized(center);radius_=radiusMeters;hysteresis_=hysteresisMeters;
  points_.reset();count_=0;kind_=Kind::Circle;initialized_=false;inside_=false;return Status::Ok;
}

Status GeoFence::setPolygon(const GeoPoint *points,size_t count,double hysteresisMeters) {
  if(!points||count<3||!std::isfinite(hysteresisMeters)||hysteresisMeters<0||hysteresisMeters>Pi*EarthRadius)
    return Status::InvalidArgument;
  if(count>MaximumPoints) return Status::TooLarge;
  Vector pivot{0,0,0};
  for(size_t i=0;i<count;++i) {
    if(!validCoordinate(points[i])) return Status::InvalidArgument;
  }
  if(distanceMeters(points[0],points[count-1])<1e-6) --count;
  if(count<3) return Status::InvalidArgument;
  for(size_t i=0;i<count;++i) { const Vector v=vector(points[i]);pivot.x+=v.x;pivot.y+=v.y;pivot.z+=v.z; }
  if(norm(pivot)<1e-10) return Status::InvalidArgument;
  const GeoPoint center=coordinate(pivot);
  const Plane plane(center);
  std::unique_ptr<PlanePoint[]> projected(new(std::nothrow) PlanePoint[count]);
  std::unique_ptr<GeoPoint[]> copy(new(std::nothrow) GeoPoint[count]);
  if(!projected||!copy) return Status::NoMemory;
  double scale=1e-12;
  for(size_t i=0;i<count;++i) {
    copy[i]=normalized(points[i]);
    if(!plane.project(copy[i],projected[i])) return Status::InvalidArgument;
    scale=std::max(scale,std::max(std::abs(projected[i].x),std::abs(projected[i].y)));
    if(distanceMeters(copy[i],points[(i+1)%count])<1e-6) return Status::InvalidArgument;
  }
  const double epsilon=scale*scale*1e-12;
  double area=0;
  for(size_t i=0;i<count;++i) {
    const size_t next=(i+1)%count;
    area+=projected[i].x*projected[next].y-projected[next].x*projected[i].y;
    for(size_t j=i+1;j<count;++j) {
      const size_t end=(j+1)%count;
      if(next==j||end==i) continue;
      if(intersects(projected[i],projected[next],projected[j],projected[end],epsilon))
        return Status::InvalidArgument;
    }
  }
  if(std::abs(area)<=epsilon) return Status::InvalidArgument;
  points_=std::move(copy);count_=count;center_=center;hysteresis_=hysteresisMeters;
  kind_=Kind::Polygon;initialized_=false;inside_=false;return Status::Ok;
}

void GeoFence::reset() {
  points_.reset();count_=0;kind_=Kind::None;initialized_=false;inside_=false;
}

Status GeoFence::update(GeoPoint position,FenceResult &result) {
  if(!validCoordinate(position)) return Status::InvalidArgument;
  if(kind_==Kind::None) return Status::NotFound;
  bool rawInside=false;double boundary=0;
  if(kind_==Kind::Circle) {
    const double distance=distanceMeters(center_,position);
    rawInside=distance<=radius_;boundary=std::abs(distance-radius_);
  } else {
    boundary=std::numeric_limits<double>::infinity();
    const Plane plane(center_);PlanePoint p{};
    const bool projectable=plane.project(position,p);
    for(size_t i=0;i<count_;++i) {
      const size_t j=(i+1)%count_;
      boundary=std::min(boundary,distanceToSegmentMeters(position,points_[i],points_[j]));
      if(!projectable) continue;
      PlanePoint a{},b{};plane.project(points_[i],a);plane.project(points_[j],b);
      if((a.y>p.y)!=(b.y>p.y) && p.x<(b.x-a.x)*(p.y-a.y)/(b.y-a.y)+a.x) rawInside=!rawInside;
    }
    if(boundary<1e-6) rawInside=true;
  }
  FenceResult candidate;candidate.distanceToBoundaryMeters=boundary;
  candidate.inside=initialized_?inside_:rawInside;
  if(initialized_&&rawInside!=inside_&&boundary>=hysteresis_) {
    candidate.inside=rawInside;candidate.entered=rawInside;candidate.exited=!rawInside;
  }
  initialized_=true;inside_=candidate.inside;result=candidate;return Status::Ok;
}

Status TrackReplay::setTrack(const TimedPoint *points,size_t count) {
  if(!points||!count||points[0].timeMs!=0) return Status::InvalidArgument;
  if(count>MaximumPoints) return Status::TooLarge;
  for(size_t i=0;i<count;++i)
    if(!validCoordinate(points[i].position)||(i&&points[i].timeMs<=points[i-1].timeMs))
      return Status::InvalidArgument;
  std::unique_ptr<TimedPoint[]> copy(new(std::nothrow) TimedPoint[count]);
  if(!copy) return Status::NoMemory;
  for(size_t i=0;i<count;++i) { copy[i]=points[i];copy[i].position=normalized(copy[i].position); }
  std::unique_ptr<double[]> headings(count>1?new(std::nothrow) double[count-1]:nullptr);
  if(headings)for(size_t i=1;i<count;++i)headings[i-1]=initialBearingDegrees(copy[i-1].position,copy[i].position);
  points_=std::move(copy);segmentHeadings_=std::move(headings);count_=count;return Status::Ok;
}

Status TrackReplay::sample(uint32_t elapsedMs,ReplaySample &sample,bool loop) const {
  if(!count_) return Status::NotFound;
  ReplaySample result;
  if(count_==1) { result.position=points_[0].position;result.finished=true;sample=result;return Status::Ok; }
  const uint32_t duration=points_[count_-1].timeMs;
  if(loop) elapsedMs%=duration;
  if(!loop&&elapsedMs>=duration) {
    result.position=points_[count_-1].position;result.segment=count_-2;result.finished=true;
    result.headingDegrees=segmentHeadings_?segmentHeadings_[count_-2]:initialBearingDegrees(points_[count_-2].position,result.position);
  } else {
    size_t low=0,high=count_-1;
    while(low+1<high) { const size_t middle=low+(high-low)/2;if(points_[middle].timeMs<=elapsedMs) low=middle;else high=middle; }
    const auto &a=points_[low];const auto &b=points_[low+1];
    const double ratio=double(elapsedMs-a.timeMs)/double(b.timeMs-a.timeMs);
    result.position={a.position.latitude+(b.position.latitude-a.position.latitude)*ratio,
      normalizeLongitude(a.position.longitude+normalizeLongitude(b.position.longitude-a.position.longitude)*ratio)};
    result.headingDegrees=segmentHeadings_?segmentHeadings_[low]:initialBearingDegrees(a.position,b.position);result.segment=low;
  }
  if(!std::isfinite(result.headingDegrees)) result.headingDegrees=0;
  sample=result;return Status::Ok;
}

} // namespace nice_gfx_map
