target_sources(${amr_wind_lib_name}
  PRIVATE

  BoussinesqBubble.cpp
  BoussinesqBubbleFieldInit.cpp
  ChannelFlow.cpp
  RayleighTaylor.cpp
  RayleighTaylorFieldInit.cpp
  TaylorGreenVortex.cpp
  FreeStream.cpp
  ConvectingTaylorVortex.cpp
  EkmanSpiral.cpp
  SyntheticTurbulence.cpp
  )

add_subdirectory(multiphase)
add_subdirectory(udfs)
