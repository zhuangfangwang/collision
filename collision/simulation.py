import numpy as np

import  collision.Collision as spatial
from collision.mesh import Mesh, icosphere
from collision import math


class Actor(object):
    def __init__(self, mesh):
        self.mesh = mesh

        self.position = self.mesh.vertices      # alias mesh vertices
        self.force = np.zeros_like(self.position)
        self.length_scale = float(mesh.edge_lengths().mean())

        self.gravity = np.array([0, 0, -1])

    def integrate(self, dt):
        pass

    def get_spatial_grid(self):
        return spatial.Grid3d(
            self.position,
            self.length_scale * 2
        )
    def get_spatial_mesh(self):
        return spatial.Mesh(
            self.position,
            self.mesh.vertex_normals(),
            self.mesh.faces,
            self.length_scale / 2
        )
    # def spatial_collide(self, other):
    #     spatial.Info(self.get_spatial_grid(), other.get_spatial_mesh())
    def permute(self):
        """apply permutation of previously computed grid to all state variables"""

    # def compute_forces(self):
    #     self.force[:] = self.gravity

    def collect_forces(self):
        self.force[:] = 0


class StaticActor(Actor):
    def __init__(self, mesh):
        super(StaticActor, self).__init__(mesh)
        grid = self.get_spatial_grid()
        mesh = self.get_spatial_mesh()
        self.get_spatial_grid = lambda : grid
        self.get_spatial_mesh = lambda : mesh

class RigidActor(Actor):
    """integrates all external forces into angular momentum quaternion"""
    pass

class Nonridid(Actor):

    def __init__(self, mesh, elasticity, damping):
        super(Nonridid, self).__init__(mesh)
        self.vertex_incidence = inc = self.mesh.compute_vertex_incidence()  # ExV
        self.elasticity = elasticity
        self.damping = damping

        self.velocity = np.zeros_like(self.position)
        self.mass = self.mesh.vertex_areas()

        self.compute_rest_length()

    def compute_rest_length(self):
        edges = self.vertex_incidence * self.mesh.vertices
        self.rest_length = np.linalg.norm(edges, axis=1)

    def compute_edge_forces(self):
        edges = self.vertex_incidence * self.position
        compression = self.rest_length - np.linalg.norm(edges, axis=1)
        dir = math.normalize(edges)
        relative_velocity = self.vertex_incidence * self.velocity
        relative_velocity = math.dot(relative_velocity, dir)
        force = compression * self.elasticity - relative_velocity * self.damping
        return self.vertex_incidence.T * (dir * force[:, None])

    def integrate(self, dt):
        self.velocity += self.force * dt / self.mass[:, None]
        self.position += self.velocity * dt

    def collect_forces(self):
        super(Nonridid, self).collect_forces()
        self.force += self.gravity * self.mass[:, None]
        self.force += self.compute_edge_forces()


class Balloon(Nonridid):

    def __init__(self, mesh, elasticity, damping, compressibility):
        super(Balloon, self).__init__(mesh, elasticity, damping)
        self.compressibility = compressibility
        self.compute_rest_volume()

    def compute_rest_volume(self):
        self.rest_volume = self.mesh.volume() * 3

    def compute_volume_forces(self):
        compression = self.rest_volume - self.mesh.volume()
        volume_gradient = self.mesh.vertex_volume_gradient()
        return volume_gradient * (compression * self.compressibility)

    def collect_forces(self):
        super(Balloon, self).collect_forces()
        self.force += self.compute_volume_forces()


class Scene(object):
    def __init__(self, actors):
        self.actors = actors

    def integrate(self, dt):
        """integrate the state of the scene with a timestep dt"""
        for a in self.actors:
            a.collect_forces()
        self.collide()
        for a in self.actors:
            a.integrate(dt)

    def collide(self):
        grids = [a.get_spatial_grid() for a in self.actors]
        meshes = [a.get_spatial_mesh() for a in self.actors]

        for i, ai in enumerate(self.actors):
            for j, aj in enumerate(self.actors):
                if i == j: continue
                info = spatial.Info(grids[i], meshes[j], False)
                mask = info.triangle != -1
                if np.any(mask):
                    force = info.depth[mask, None] * info.normal[mask]
                    assert not np.any(np.isnan(force))
                    ai.force[mask] += force * 1e-9

                    # aj.force
                    # print(info.triangle[mask])
                    # print(info.depth[mask])

    def plot(self):

        from vispy import app, scene, io

        # Prepare canvas
        canvas = scene.SceneCanvas(keys='interactive', size=(800, 600), show=True)
        canvas.measure_fps()

        # Set up a viewbox to display the image with interactive pan/zoom
        view = canvas.central_widget.add_view()

        vis_meshes = [scene.visuals.Mesh(
            actor.mesh.vertices,
            actor.mesh.faces[:, ::-1],
            shading='flat',
            parent=view.scene) for actor in self.actors]

        fov = 60.
        cam1 = scene.cameras.FlyCamera(parent=view.scene, fov=fov, name='Fly')
        view.camera = cam1


        dt = 0.002
        def update(event):
            self.integrate(dt)
            for i in range(len(self.actors)):
                actor = self.actors[i]
                if not isinstance(actor, StaticActor):
                    m = vis_meshes[i]
                    m.set_data(vertices=actor.position[actor.mesh.faces])


        timer = app.Timer(interval=dt, connect=update)


        timer.start()
        app.run()


if __name__=='__main__':
    turtle = Mesh.load_stl('part0.stl')
    # normalize orientation
    u, s, v = np.linalg.svd(turtle.vertices, full_matrices=0)
    turtle.vertices = turtle.vertices.dot(v)
    turtle.faces = turtle.faces[:, ::-1]

    ico = icosphere(0.1, refinement=3)
    ball = lambda p : icosphere(0.1, p, 3)
    e = .1
    c = 100
    d = .001
    actors = [StaticActor(turtle),
              Balloon(ball([0,0,-0.5]), e, d, c),
              Balloon(ball([0,0,0]), e, d, c)]

    scene = Scene(actors)
    scene.plot()