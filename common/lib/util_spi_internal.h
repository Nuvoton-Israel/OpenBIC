/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define FMC_CS0         "fmc_cs0"
#define FMC_CS1         "fmc_cs1"
#define SPI1_CS0        "spi1_cs0"
#define SPI1_CS1        "spi1_cs1"
#define SPI2_CS0        "spi2_cs0"
#define SPI2_CS1        "spi2_cs1"

#if DT_NODE_HAS_STATUS(DT_NODELABEL(spi_spim0_cs0), okay)
#undef FMC_CS0
#define FMC_CS0 "spi_spim0_cs0"
#endif

#if DT_NODE_HAS_STATUS(DT_NODELABEL(spi_spim0_cs1), okay)
#undef FMC_CS1
#define FMC_CS1 "spi_spim0_cs1"
#endif

#if DT_NODE_HAS_STATUS(DT_NODELABEL(spi_fiu0_cs0), okay)
#undef SPI1_CS0
#define SPI1_CS0 "spi_fiu0_cs0"
#endif

#if DT_NODE_HAS_STATUS(DT_NODELABEL(spi_fiu0_cs1), okay)
#undef SPI1_CS1
#define SPI1_CS1 "spi_fiu0_cs1"
#endif

#if DT_NODE_HAS_STATUS(DT_NODELABEL(spi_spip1_cs0), okay)
#undef SPI2_CS0
#define SPI2_CS0 "spi_spip1_cs0"
#endif

#if DT_NODE_HAS_STATUS(DT_NODELABEL(spi_spip1_cs1), okay)
#undef SPI2_CS1
#define SPI2_CS1 "spi_spip1_cs1"
#endif
